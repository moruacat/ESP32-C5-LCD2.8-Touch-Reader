#include "srv_alarm.h"

#include "srv_config.h"
#include "srv_rtc.h"
#include "srv_audio.h"
#include "srv_player.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "srv_alarm";

#define ALARM_NUM           SRV_CONFIG_ALARM_NUM   /* 3 */
/* 手机闹钟习惯：无人应答持续 10 分钟才自动停，期间可「稍后提醒」 */
#define RING_AUTO_STOP_S    600
#define SNOOZE_MINUTES      5                      /* 「稍后提醒」默认间隔 */
#define CHECK_PERIOD_US     (1000 * 1000)          /* 1 Hz */

static srv_alarm_cb_t s_cb  = NULL;
static void          *s_arg = NULL;
static esp_timer_handle_t s_timer;

static bool s_ringing[ALARM_NUM];
static int  s_ring_left[ALARM_NUM];      /* 剩余响铃秒数 */
static int  s_snooze_at[ALARM_NUM];      /* 稍后提醒的「当天第几分钟」，-1 = 无 */
static int  s_last_min = -1;             /* 上次检查到的「当天第几分钟」，防同一分钟重复触发 */

/* ===================== 响铃 ===================== */

static void ring_task(void *arg)
{
    int idx = (int)(intptr_t)arg;

    /* 响铃优先：先停掉音乐，把音频通道让出来 */
    if (srv_player_state() != SRV_PLAYER_IDLE) {
        (void)srv_player_stop();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    int vol_backup = srv_audio_get_volume();
    const int vol_max = srv_audio_get_alarm_volume();
    const int vol_start = (vol_max >= 30) ? (vol_max / 3) : vol_max;
    srv_audio_set_volume(vol_start);

    ESP_LOGW(TAG, "闹钟 %d 响铃", idx + 1);

    /*
     * 手机闹钟的节奏：不是每 0.5 秒一直叫，而是隔一会儿「叮」一声；
     * 音量从 1/3 开始，30 秒内渐强到设定音量，避免一上来就吓人。
     */
    while (s_ringing[idx]) {
        const int elapsed = RING_AUTO_STOP_S - s_ring_left[idx];
        int vol = vol_start + (vol_max - vol_start) * (elapsed < 30 ? elapsed : 30) / 30;
        if (vol > vol_max) {
            vol = vol_max;
        }
        srv_audio_set_volume(vol);
        srv_audio_beep(2000, 400);
        vTaskDelay(pdMS_TO_TICKS(1600));

        if (s_ring_left[idx] <= 0) {
            break;
        }
    }

    s_ringing[idx] = false;
    srv_audio_set_volume(vol_backup);
    ESP_LOGI(TAG, "闹钟 %d 停止", idx + 1);

    vTaskDelete(NULL);
}

static void trigger(int idx)
{
    if (idx < 0 || idx >= ALARM_NUM || s_ringing[idx]) {
        return;
    }

    s_ringing[idx]  = true;
    s_ring_left[idx] = RING_AUTO_STOP_S;
    s_snooze_at[idx] = -1;              /* 正常触发时清掉旧的「稍后提醒」预约 */

    if (s_cb) {
        s_cb(idx, s_arg);
    }

    if (xTaskCreate(ring_task, "alarm_ring", 4096, (void *)(intptr_t)idx, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "响铃任务创建失败");
        s_ringing[idx] = false;
    }
}

esp_err_t srv_alarm_stop(int index)
{
    int idx = index - 1;
    if (idx < 0 || idx >= ALARM_NUM) {
        return ESP_ERR_INVALID_ARG;
    }
    s_ringing[idx] = false;
    s_snooze_at[idx] = -1;              /* 「停止」也取消稍后提醒 */
    return ESP_OK;
}

esp_err_t srv_alarm_snooze(int index, int minutes)
{
    int idx = index - 1;
    if (idx < 0 || idx >= ALARM_NUM || minutes <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    s_ringing[idx] = false;             /* 先停这一次 */

    srv_rtc_time_t t;
    int now = 0;
    if (srv_rtc_get_time(&t) == ESP_OK) {
        now = t.hour * 60 + t.min;
    }
    const int at = (now + minutes) % (24 * 60);
    s_snooze_at[idx] = at;
    ESP_LOGI(TAG, "闹钟 %d 稍后 %d 分钟（%02d:%02d 再响）", index, minutes, at / 60, at % 60);
    return ESP_OK;
}

bool srv_alarm_is_ringing(int index)
{
    int idx = index - 1;
    if (idx < 0 || idx >= ALARM_NUM) {
        return false;
    }
    return s_ringing[idx];
}

/* ===================== 调度 ===================== */

int srv_alarm_check_now(void)
{
    srv_rtc_time_t t;
    if (srv_rtc_get_time(&t) != ESP_OK) {
        return 0;
    }

    int now_min = t.hour * 60 + t.min;
    int triggered = 0;

    /* 响铃倒计时 */
    for (int i = 0; i < ALARM_NUM; i++) {
        if (s_ringing[i] && s_ring_left[i] > 0) {
            s_ring_left[i]--;
        } else if (s_ringing[i]) {
            s_ringing[i] = false;
        }
    }

    /* 稍后提醒：到点重新响。每秒检查，触发后清标记，不会重复 */
    for (int i = 0; i < ALARM_NUM; i++) {
        if (s_snooze_at[i] >= 0 && now_min >= s_snooze_at[i]) {
            s_snooze_at[i] = -1;
            trigger(i);
            triggered = i + 1;
        }
    }

    /* 同一分钟内只判定一次（snooze 不重复响） */
    if (now_min == s_last_min) {
        return triggered;
    }
    s_last_min = now_min;

    for (int i = 0; i < ALARM_NUM; i++) {
        char key[16];
        snprintf(key, sizeof(key), "alarm%d_en", i + 1);
        if (srv_config_get_int(key, 0) == 0) {
            continue;
        }
        snprintf(key, sizeof(key), "alarm%d_h", i + 1);
        int h = (int)srv_config_get_int(key, 0);
        snprintf(key, sizeof(key), "alarm%d_m", i + 1);
        int m = (int)srv_config_get_int(key, 0);

        if (h == t.hour && m == t.min) {
            trigger(i);
            triggered = i + 1;
            break;
        }
    }

    /* 时段静音不在这里判：它只在「进音乐页」时应用一次（见 ui_music.c），
     * 之后不再跟着时间变，所以不需要每分钟轮询。 */

    return triggered;
}

static void alarm_timer_cb(void *arg)
{
    (void)arg;
    srv_alarm_check_now();
}

esp_err_t srv_alarm_register_cb(srv_alarm_cb_t cb, void *arg)
{
    s_cb  = cb;
    s_arg = arg;
    return ESP_OK;
}

esp_err_t srv_alarm_init(void)
{
    if (s_timer) {
        return ESP_OK;
    }

    const esp_timer_create_args_t args = {
        .callback = alarm_timer_cb,
        .name     = "alarm_tick",
    };
    esp_err_t ret = esp_timer_create(&args, &s_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "定时器创建失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_timer_start_periodic(s_timer, CHECK_PERIOD_US);
    if (ret != ESP_OK) {
        return ret;
    }

    /* -1 表示「没有预约稍后提醒」；static 默认 0 会被当成 00:00 误触发 */
    for (int i = 0; i < ALARM_NUM; i++) {
        s_snooze_at[i] = -1;
    }

    ESP_LOGI(TAG, "闹钟调度就绪（%d 组，每秒轮询 RTC）", ALARM_NUM);
    return ESP_OK;
}
