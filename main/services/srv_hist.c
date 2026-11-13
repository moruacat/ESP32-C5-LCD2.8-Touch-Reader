#include "srv_hist.h"
#include "srv_battery.h"
#include "srv_sensors.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "srv_hist";

static srv_hist_point_t s_buf[SRV_HIST_POINTS];
static int      s_count = 0;      /*!< 有效点数，满后不再增长 */
static int      s_next  = 0;      /*!< 下一个写入位置（环形） */
static uint32_t s_seq   = 0;

static void sample_once(void)
{
    srv_hist_point_t p;
    memset(&p, 0, sizeof(p));
    p.uptime_s    = (uint32_t)(esp_timer_get_time() / 1000000);
    p.battery_pct = -1;
    p.temp_c10    = INT16_MIN;

    float volt = 0.0f;
    int   pct  = 0;
    if (srv_battery_get(&volt, &pct) == ESP_OK) {
        p.battery_pct = (int16_t)pct;
        p.battery_mv  = (uint16_t)(volt * 1000.0f + 0.5f);
    }

    float temp = 0.0f;
    float humi = 0.0f;
    if (srv_sensors_get_temp_humi(&temp, &humi) == ESP_OK) {
        p.temp_c10 = (int16_t)(temp * 10.0f + (temp >= 0.0f ? 0.5f : -0.5f));
        p.humi_p10 = (uint16_t)(humi * 10.0f + 0.5f);
    }

    s_buf[s_next] = p;
    s_next = (s_next + 1) % SRV_HIST_POINTS;
    if (s_count < SRV_HIST_POINTS) {
        s_count++;
    }
    s_seq++;
}

static void hist_task(void *arg)
{
    (void)arg;

    sample_once();          /* 开机先采一次，别让页面空着等一分钟 */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SRV_HIST_INTERVAL_S * 1000));
        sample_once();
    }
}

esp_err_t srv_hist_init(void)
{
    static bool started = false;
    if (started) {
        return ESP_OK;
    }
    if (xTaskCreate(hist_task, "srv_hist", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "采样任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    started = true;
    ESP_LOGI(TAG, "运行时历史就绪：每 %d 秒一点，共 %d 点（约 %d 分钟）",
             SRV_HIST_INTERVAL_S, SRV_HIST_POINTS,
             SRV_HIST_INTERVAL_S * SRV_HIST_POINTS / 60);
    return ESP_OK;
}

int srv_hist_count(void)
{
    return s_count;
}

uint32_t srv_hist_seq(void)
{
    return s_seq;
}

int srv_hist_get(srv_hist_point_t *out, int max)
{
    if (out == NULL || max <= 0 || s_count <= 0) {
        return 0;
    }
    int n = (s_count < max) ? s_count : max;

    /* 最旧的一个：s_next 指向下一个待写位置，往回数 s_count 个 */
    int start = (s_next - s_count + SRV_HIST_POINTS) % SRV_HIST_POINTS;
    if (s_count > n) {
        /* 只要最近的 n 个 */
        start = (start + (s_count - n)) % SRV_HIST_POINTS;
    }
    for (int i = 0; i < n; i++) {
        out[i] = s_buf[(start + i) % SRV_HIST_POINTS];
    }
    return n;
}
