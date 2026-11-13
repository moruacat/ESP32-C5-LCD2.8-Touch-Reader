#include "app_init.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp32_c5_touch_lcd_2.8.h"
#include "bsp/display.h"
#include "srv_sd.h"
#include "srv_config.h"
#include "srv_sensors.h"
#include "srv_motion.h"
#include "srv_rtc.h"
#include "srv_battery.h"
#include "srv_input.h"
#include "srv_wifi.h"
#include "srv_ntp.h"
#include "srv_hist.h"
#include "srv_audio.h"
#include "srv_alarm.h"
#include "srv_timer.h"
#include "srv_recorder.h"
#include "srv_photo.h"
#include "srv_musiclib.h"
#include "srv_player.h"
#include "app_navigator.h"
#include "ui_alarm_alert.h"
#include "ui_ime.h"
#include "ui_sleep.h"

static const char *TAG = "app_init";

/* ===================== 开机联网的延迟 ===================== */

/*! 开机后等多久再联网。
 *
 *  C5 是单核（CONFIG_FREERTOS_UNICORE=y），而 WiFi 驱动任务是优先级 23 的
 *  高优先级任务；srv_wifi_set_enabled() 里 esp_wifi_start() 返回后紧接着就
 *  srv_wifi_autoconnect() 开始扫描，扫描要持续十几秒。这段期间 LVGL 任务
 *  几乎抢不到 CPU —— 开机动画帧率掉到 70% 上下，前几个方块的飞入整段丢掉，
 *  看起来就是「卡片瞬移」。
 *
 *  所以开机不立刻联网：等动画（含起始延迟约 3.1s）放完再开射频。
 *  代价是开机后晚 4 秒才有网，换来动画全程满帧。 */
#define BOOT_NET_DELAY_MS  4000

static void boot_net_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(BOOT_NET_DELAY_MS));

    /* 期间用户可能已经在设置里手动开过网/改过配置，这里再确认一次 */
    if (srv_config_get_int(CFG_WIFI_EN, 0) != 0) {
        srv_wifi_set_enabled(true);     /* 内部会按网络历史自动连接 */
        srv_ntp_autostart();
    } else {
        ESP_LOGI(TAG, "网络开关为关（延迟检查），跳过联网与 NTP");
    }

    vTaskDelete(NULL);
}

/**
 * @brief LCD CS 上电拉高，避免 SD 卡 SPI 操作期间 LCD 被误片选
 */
static void lcd_cs_gpio_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BSP_LCD_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(BSP_LCD_CS, 1);
}

void app_init_all(void)
{
    /* 1. NVS（WiFi 校准等系统数据） */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. LCD CS 拉高，防止 SPI 总线冲突 */
    lcd_cs_gpio_init();

    /* 3. CH32V003 IO 扩展（LCD/触摸复位、背光 PWM、功放） */
    ESP_ERROR_CHECK(bsp_io_expander_init());
    vTaskDelay(pdMS_TO_TICKS(100));

    /* 4. SD 卡（失败不阻塞启动） */
    srv_sd_init();

    /* 4.1 配置读取（/config.dat，缺失或损坏时用默认值） */
    srv_config_init();

    /* 4.2 中文输入法自检：把「SD 卡上的词库/字库在不在、连续内存够不够」先打一行到日志上。
     *     只看文件和内存，不加载字体（那要好几 MB 连续块，不能为了自检白占着）；
     *     放在 SD 卡挂载之后，否则文件都读不到会误报缺文件。 */
    ui_ime_diag_log();

    /* 5. 传感器与 RTC */
    srv_sensors_init();
    srv_rtc_init();

    /* 5.0.1 重力感应（复用上面初始化好的 QMI8658，给重力球用）。
     *       传感器没起来只会让重力类功能不可用，不阻塞启动。 */
    srv_motion_init();

    /* 5.0 温湿度记录：按配置开关决定是否启动（每 10 分钟写一行到
     *     /temp-rec/YYYY/MM/DD.csv）。开关在「设置 → 记录」里，存 /config.dat，
     *     所以关掉之后重启也不会自己又开起来。 */
    if (srv_config_get_int(CFG_TEMP_REC_EN, 1) != 0) {
        srv_sensors_record_start(10);
    } else {
        ESP_LOGI(TAG, "温湿度记录开关为关，跳过记录任务");
    }

    /* 5.1 电池监测（CH32 ADC） */
    srv_battery_init();

    /* 5.1.1 运行时采样：60 秒记一次电池/温湿度，供电池页和环境页画曲线。
     *       只存内存（90 点 ≈ 1.5 小时），不读也不写历史 CSV。 */
    srv_hist_init();

    /* 5.2 音频（I2S + ES8311，音量取自配置） */
    srv_audio_init();

    /* 5.3 闹钟调度（1Hz 轮询 RTC，同时驱动时段静音） */
    srv_alarm_init();

    /* 5.4 计时器 / 倒计时（状态的持有者，切页后仍继续走时） */
    srv_timer_init();

    /* 5.5 录音（复用 srv_audio 建好的全双工 I2S，挂 ES8311 录音通路） */
    srv_recorder_init();

    /* 5.6 音乐库 + 播放器（扫描 /music，恢复上次播放位置，不自动出声） */
    srv_musiclib_init();
    srv_player_init();
    srv_player_restore();

    /* 5.6.1 照片库（扫描 /photos 下的 .raw，进页面时还会再扫一次） */
    srv_photo_init();

    /* 5.7 网络与校时（只初始化，是否开启射频由配置决定） */
    srv_wifi_init();
    srv_ntp_init();

    /* 6. 显示 + LVGL（内部创建 LVGL 任务） */
    if (bsp_display_start() == NULL) {
        ESP_LOGE(TAG, "display start failed");
    }
    bsp_display_brightness_init();
    bsp_display_brightness_set((int)srv_config()->brightness);

    /* 7. BOOT 键 / 闹钟提示 / 休眠，全部在建页面之前备好。
     *    开机动画在下一步的 app_navigator_init() 里启动，动画一旦开跑，
     *    后面就不该再有初始化来抢单核 CPU（见下面第 8 步的说明）。 */
    srv_input_init();                   /* BOOT 键：GPIO28 轮询 + 去抖 + 状态机 */
    ui_alarm_alert_init();              /* 闹钟响铃提示（注册回调，不建对象） */
    ui_sleep_init();                    /* 休眠层 + 自动变暗/锁屏（持锁改对象树） */

    /* 8. 导航器 + UI 初始化：装主题、进时钟页、紧接着启动开机动画。
     *    放最后：动画开跑之后不希望再有初始化来抢 CPU 或抢 LVGL 锁。 */
    app_navigator_init();

    /* 9. 联网：交给延迟任务，等动画放完再开射频。
     *    原因见 boot_net_task() 上面的说明（WiFi 驱动优先级 23 + C5 单核，
     *    一扫描就把 LVGL 的 CPU 抢光，前几个方块会瞬移）。 */
    if (srv_config_get_int(CFG_WIFI_EN, 0) != 0) {
        if (xTaskCreate(boot_net_task, "boot_net", 4096, NULL, 3, NULL) != pdPASS) {
            ESP_LOGW(TAG, "联网任务创建失败，改为立即联网");
            srv_wifi_set_enabled(true);
            srv_ntp_autostart();
        }
    } else {
        ESP_LOGI(TAG, "网络开关为关，跳过联网与 NTP");
    }

    ESP_LOGI(TAG, "system init done");
}
