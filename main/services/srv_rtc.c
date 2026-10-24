#include "srv_rtc.h"
#include "esp_log.h"
#include "esp_err.h"
#include "bsp/esp32_c5_touch_lcd_2.8.h"
#include "pcf85063a.h"

static const char *TAG = "srv_rtc";

/* BSP 创建的 PCF85063A 设备（非 static 全局） */
extern pcf85063a_dev_t pcf_dev;

static bool s_rtc_ok = false;

esp_err_t srv_rtc_init(void)
{
    /* 复用 BSP 初始化（不再强制写入固定时间） */
    esp_err_t ret = pcf_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063A init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    srv_rtc_time_t t = {0};
    if (srv_rtc_get_time(&t) != ESP_OK || !srv_rtc_time_valid(&t)) {
        /* RTC 从未初始化（无电池），设置默认时间 2026-01-01 00:00:00 周四 */
        t.year = 2026;
        t.month = 1;
        t.day = 1;
        t.dotw = 4;
        t.hour = 0;
        t.min = 0;
        t.sec = 0;
        srv_rtc_set_time(&t);
        ESP_LOGW(TAG, "RTC time invalid, set default 2026-01-01");
    }
    s_rtc_ok = true;
    ESP_LOGI(TAG, "PCF85063A RTC ready");
    return ESP_OK;
}

esp_err_t srv_rtc_get_time(srv_rtc_time_t *t)
{
    if (!t) {
        return ESP_ERR_INVALID_ARG;
    }
    pcf85063a_datetime_t dt = {0};
    esp_err_t ret = pcf85063a_get_time_date(&pcf_dev, &dt);
    if (ret != ESP_OK) {
        return ret;
    }
    t->year = dt.year;
    t->month = dt.month;
    t->day = dt.day;
    t->dotw = dt.dotw;
    t->hour = dt.hour;
    t->min = dt.min;
    t->sec = dt.sec;
    return ESP_OK;
}

esp_err_t srv_rtc_set_time(const srv_rtc_time_t *t)
{
    if (!t) {
        return ESP_ERR_INVALID_ARG;
    }
    pcf85063a_datetime_t dt = {
        .year = t->year,
        .month = t->month,
        .day = t->day,
        .dotw = t->dotw,
        .hour = t->hour,
        .min = t->min,
        .sec = t->sec,
    };
    return pcf85063a_set_time_date(&pcf_dev, dt);
}

int srv_rtc_calc_dotw(int year, int month, int day)
{
    /* Sakamoto 算法：0=周日 */
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = year;
    if (month < 3) {
        y -= 1;
    }
    return (y + y / 4 - y / 100 + y / 400 + t[month - 1] + day) % 7;
}

bool srv_rtc_time_valid(const srv_rtc_time_t *t)
{
    if (!t) {
        return false;
    }
    return (t->year >= 2024 && t->year <= 2099);
}
