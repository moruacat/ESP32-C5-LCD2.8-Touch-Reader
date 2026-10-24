#include "ui_alarm_alert.h"

#include "ui_dialog.h"
#include "ui_common.h"
#include "srv_alarm.h"
#include "srv_config.h"

#include "bsp/esp32_c5_touch_lcd_2.8.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>

static const char *TAG = "alarm_alert";

/* 键名与标题走常量表（snprintf 拼 "alarm%d_h" 会被 -Werror=format-truncation 拦） */
static const char *const ALARM_TITLE[SRV_CONFIG_ALARM_NUM] = {"闹钟 1", "闹钟 2", "闹钟 3"};
static const char *const K_H[SRV_CONFIG_ALARM_NUM]         = {"alarm1_h", "alarm2_h", "alarm3_h"};
static const char *const K_M[SRV_CONFIG_ALARM_NUM]         = {"alarm1_m", "alarm2_m", "alarm3_m"};

static int          s_ringing_idx;      /*!< 正在响的闹钟 1..3，0 = 无 */
static lv_timer_t  *s_watch_timer;      /*!< 1Hz 盯着响铃结束，顺便补弹提示层 */

/* ===================== 提示层 ===================== */

static void stop_btn_cb(void *arg)
{
    srv_alarm_stop((int)(intptr_t)arg);
}

static void show_ring_alert(int index)
{
    char note[16];

    snprintf(note, sizeof(note), "%02d:%02d",
             (int)srv_config_get_int(K_H[index - 1], 0),
             (int)srv_config_get_int(K_M[index - 1], 0));

    /* 不自动关：60 秒后 srv_alarm 自己会停，下面的 watch 定时器再把提示收掉 */
    ui_alert_show(ALARM_TITLE[index - 1], UI_COLOR_TEMP, note,
                  "停止", stop_btn_cb, (void *)(intptr_t)index, 0);
}

/**
 * @brief 响铃期间每秒检查一次
 *
 * 两件事：铃停了就收掉提示层；提示层被切页带走（它挂在屏幕上，换页会跟着销毁）
 * 而铃还在响，就补弹一次 —— 只在这种情况下重建，不会每秒闪一次。
 */
static void watch_timer_cb(lv_timer_t *timer)
{
    int idx = s_ringing_idx;

    if (idx == 0 || !srv_alarm_is_ringing(idx)) {
        ui_alert_close();
        s_ringing_idx = 0;
        s_watch_timer = NULL;
        lv_timer_delete(timer);
        return;
    }

    if (!ui_alert_visible()) {
        show_ring_alert(idx);
    }
}

/* ===================== 响铃回调 ===================== */

static void alarm_ui_task(void *arg)
{
    int index = (int)(intptr_t)arg;

    if (bsp_display_lock(UI_LOCK_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "取 LVGL 锁失败，跳过闹钟提示");
        vTaskDelete(NULL);
        return;
    }

    s_ringing_idx = index;
    show_ring_alert(index);

    if (s_watch_timer == NULL) {
        s_watch_timer = lv_timer_create(watch_timer_cb, 1000, NULL);
    }

    bsp_display_unlock();
    vTaskDelete(NULL);
}

/* srv_alarm 的触发回调：跑在 esp_timer 上下文，不能碰 LVGL，只能转任务 */
static void on_alarm_ring(int index, void *arg)
{
    (void)arg;

    ESP_LOGW(TAG, "闹钟 %d 响铃，弹出停止提示", index);

    if (xTaskCreate(alarm_ui_task, "alarm_ui", 4096,
                    (void *)(intptr_t)(index + 1), 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "提示任务创建失败");
    }
}

/* ===================== 初始化 ===================== */

void ui_alarm_alert_init(void)
{
    srv_alarm_register_cb(on_alarm_ring, NULL);

    /* BOOT 键不在这里注册：它是单回调槽，统一交给 ui_sleep 处理
     * （那边会先判响铃、再决定要不要休眠）。 */
    ESP_LOGI(TAG, "闹钟提示就绪（点屏幕上的「停止」或按 BOOT 键）");
}
