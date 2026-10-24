#include "srv_timer.h"

#include "srv_config.h"
#include "srv_audio.h"
#include "srv_player.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stddef.h>

static const char *TAG = "srv_timer";

/*! 倒计时默认时长：5 分钟 */
#define CD_DEFAULT_MS   (5 * 60 * 1000)

/*! 到点提示音：短促三声 */
#define BEEP_TIMES      3
#define BEEP_FREQ_HZ    2000
#define BEEP_MS         200
#define BEEP_GAP_MS     150

/* 秒表与倒计时的状态都由多个上下文读写：
 * 秒表被 LVGL 任务读、倒计时还被 esp_timer 回调与响铃任务写，
 * 所以改状态时统一进临界区；读时间用 esp_timer_get_time()（纯读寄存器，可以在临界区里调）。
 * 注意 esp_timer_start/stop 内部会加锁，必须在临界区外调用。 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* ===================== 秒表 ===================== */
static bool    s_sw_running;
static int64_t s_sw_base_ms;        /*!< 暂停前累计的毫秒 */
static int64_t s_sw_start_us;       /*!< 本轮运行的起点 */

/* ===================== 倒计时 ===================== */
static bool    s_cd_running;
static bool    s_cd_finished;
static int64_t s_cd_total_ms = CD_DEFAULT_MS;
static int64_t s_cd_base_ms;        /*!< 已经走掉的毫秒 */
static int64_t s_cd_start_us;       /*!< 本轮运行的起点 */
static esp_timer_handle_t s_cd_timer;

static srv_timer_done_cb_t s_done_cb;
static void               *s_done_arg;

static inline int64_t now_us(void)
{
    return esp_timer_get_time();
}

/* ===================== 提示音开关 ===================== */

bool srv_timer_sound_enabled(void)
{
    return srv_config_get_int(CFG_TIMER_SOUND, 1) != 0;
}

void srv_timer_set_sound(bool on)
{
    srv_config_set_int(CFG_TIMER_SOUND, on ? 1 : 0, true);
}

/* ===================== 秒表 ===================== */

void srv_timer_sw_toggle(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_sw_running) {
        s_sw_base_ms += (now_us() - s_sw_start_us) / 1000;
        s_sw_running  = false;
    } else {
        s_sw_start_us = now_us();
        s_sw_running  = true;
    }
    portEXIT_CRITICAL(&s_lock);
}

void srv_timer_sw_reset(void)
{
    portENTER_CRITICAL(&s_lock);
    s_sw_running  = false;
    s_sw_base_ms  = 0;
    s_sw_start_us = 0;
    portEXIT_CRITICAL(&s_lock);
}

bool srv_timer_sw_is_running(void)
{
    return s_sw_running;
}

bool srv_timer_sw_is_active(void)
{
    return s_sw_running || s_sw_base_ms > 0;
}

int64_t srv_timer_sw_elapsed_ms(void)
{
    int64_t ms;

    portENTER_CRITICAL(&s_lock);
    ms = s_sw_base_ms;
    if (s_sw_running) {
        ms += (now_us() - s_sw_start_us) / 1000;
    }
    portEXIT_CRITICAL(&s_lock);

    return ms < 0 ? 0 : ms;
}

/* ===================== 倒计时 ===================== */

/* 响铃任务：提示音是阻塞调用（I2S 写），不能在 esp_timer 回调里做，
 * 也不能在 LVGL 任务里做（会卡住界面），所以单独起任务。 */
static void cd_ring_task(void *arg)
{
    (void)arg;

    if (srv_timer_sound_enabled()) {
        /* 提示音优先：先停音乐腾出音频通道 */
        if (srv_player_state() != SRV_PLAYER_IDLE) {
            (void)srv_player_stop();
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        int vol_backup = srv_audio_get_volume();
        srv_audio_set_volume(srv_audio_get_alarm_volume());

        for (int i = 0; i < BEEP_TIMES; i++) {
            srv_audio_beep(BEEP_FREQ_HZ, BEEP_MS);
            vTaskDelay(pdMS_TO_TICKS(BEEP_GAP_MS));
        }

        srv_audio_set_volume(vol_backup);
    } else {
        ESP_LOGI(TAG, "提示音已关闭，仅屏幕提示");
    }

    if (s_done_cb != NULL) {
        s_done_cb(s_done_arg);
    }

    vTaskDelete(NULL);
}

/* 到点：停表、置结束标志，再起一个任务响提示音并通知上层。
 * esp_timer 回调里不能做阻塞动作（响铃、取 LVGL 锁），所以必须转任务。 */
static void cd_once_cb(void *arg)
{
    (void)arg;

    portENTER_CRITICAL(&s_lock);
    s_cd_running  = false;
    s_cd_finished = true;
    s_cd_base_ms  = s_cd_total_ms;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGW(TAG, "倒计时到点");

    if (xTaskCreate(cd_ring_task, "cd_ring", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "响铃任务创建失败");
    }
}

void srv_timer_cd_set_ms(int64_t ms)
{
    if (ms <= 0) {
        ms = 1000;                      /* 0 秒没有意义，按 1 秒处理 */
    }

    esp_timer_stop(s_cd_timer);         /* 未启动时返回错误，忽略即可 */

    portENTER_CRITICAL(&s_lock);
    s_cd_total_ms = ms;
    s_cd_base_ms  = 0;
    s_cd_start_us = 0;
    s_cd_running  = false;
    s_cd_finished = false;
    portEXIT_CRITICAL(&s_lock);
}

void srv_timer_cd_set_hms(int h, int m, int s)
{
    if (h < 0) h = 0;
    if (m < 0) m = 0;
    if (s < 0) s = 0;

    srv_timer_cd_set_ms(((int64_t)h * 3600 + (int64_t)m * 60 + s) * 1000);
}

int64_t srv_timer_cd_total_ms(void)
{
    return s_cd_total_ms;
}

void srv_timer_cd_toggle(void)
{
    /* 暂停 */
    if (s_cd_running) {
        portENTER_CRITICAL(&s_lock);
        s_cd_base_ms += (now_us() - s_cd_start_us) / 1000;
        s_cd_running  = false;
        portEXIT_CRITICAL(&s_lock);

        esp_timer_stop(s_cd_timer);
        return;
    }

    /* 开始 / 继续；上一轮已结束时用原时长重新开始 */
    int64_t remain_us;

    portENTER_CRITICAL(&s_lock);
    if (s_cd_finished || s_cd_base_ms >= s_cd_total_ms) {
        s_cd_finished = false;
        s_cd_base_ms  = 0;
    }
    s_cd_start_us = now_us();
    s_cd_running  = true;
    remain_us     = (s_cd_total_ms - s_cd_base_ms) * 1000;
    portEXIT_CRITICAL(&s_lock);

    if (remain_us <= 0) {
        remain_us = 1000;
    }

    esp_timer_stop(s_cd_timer);
    esp_timer_start_once(s_cd_timer, (uint64_t)remain_us);
}

void srv_timer_cd_reset(void)
{
    esp_timer_stop(s_cd_timer);

    portENTER_CRITICAL(&s_lock);
    s_cd_running  = false;
    s_cd_finished = false;
    s_cd_base_ms  = 0;
    s_cd_start_us = 0;
    portEXIT_CRITICAL(&s_lock);
}

bool srv_timer_cd_is_running(void)
{
    return s_cd_running;
}

bool srv_timer_cd_is_active(void)
{
    return s_cd_running || (s_cd_base_ms > 0 && !s_cd_finished);
}

bool srv_timer_cd_is_finished(void)
{
    return s_cd_finished;
}

int64_t srv_timer_cd_remaining_ms(void)
{
    int64_t used;
    int64_t left;

    portENTER_CRITICAL(&s_lock);
    used = s_cd_base_ms;
    if (s_cd_running) {
        used += (now_us() - s_cd_start_us) / 1000;
    }
    left = s_cd_total_ms - used;
    portEXIT_CRITICAL(&s_lock);

    return left < 0 ? 0 : left;
}

/* ===================== 初始化 ===================== */

esp_err_t srv_timer_init(void)
{
    if (s_cd_timer != NULL) {
        return ESP_OK;
    }

    const esp_timer_create_args_t args = {
        .callback = cd_once_cb,
        .name     = "cd_once",
    };

    esp_err_t ret = esp_timer_create(&args, &s_cd_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "倒计时定时器创建失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "计时器服务就绪");
    return ESP_OK;
}

esp_err_t srv_timer_register_done_cb(srv_timer_done_cb_t cb, void *arg)
{
    s_done_cb  = cb;
    s_done_arg = arg;
    return ESP_OK;
}
