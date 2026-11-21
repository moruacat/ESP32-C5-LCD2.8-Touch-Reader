#include "srv_lunar.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* 1900–2099 的农历年编码表（含闰月与大小月），由 tools/gen_lunar_table.py 生成 */
#include "lunar_table.inc"

#define YEAR_COUNT  ((int)(sizeof(s_year_infos) / sizeof(s_year_infos[0])))

static const char *const TIANGAN[] = {
    "甲", "乙", "丙", "丁", "戊", "己", "庚", "辛", "壬", "癸",
};

static const char *const DIZHI[] = {
    "子", "丑", "寅", "卯", "辰", "巳", "午", "未", "申", "酉", "戌", "亥",
};

static const char *const MONTH_NAME[] = {
    "", "正月", "二月", "三月", "四月", "五月", "六月",
    "七月", "八月", "九月", "十月", "十一月", "十二月",
};

static const char *const DAY_NAME[] = {
    "",
    "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
    "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
    "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十",
};

/* 公历 → 1970-01-01 起的天数（Howard Hinnant 的 days_from_civil） */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= (m <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - (int)(era * 400));
    const unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

esp_err_t srv_lunar_from_solar(int year, int month, int day, srv_lunar_date_t *out)
{
    if (out == NULL || year < 1900 || month < 1 || month > 12 || day < 1 || day > 31) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 农历起点：公历 1900-01-31 = 农历 1900 年正月初一 */
    int64_t offset = days_from_civil(year, month, day) -
                     days_from_civil(1900, 1, 31);
    if (offset < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int yi = 0;
    while (yi < YEAR_COUNT && offset >= (int64_t)s_year_days[yi]) {
        offset -= (int64_t)s_year_days[yi];
        yi++;
    }
    if (yi >= YEAR_COUNT) {
        return ESP_ERR_INVALID_ARG;     /* 超出 2099 年末 */
    }

    const uint32_t info = s_year_infos[yi];
    const int leap_month = (int)(info % 16u);

    for (int m = 1; m <= 12; m++) {
        int dm = 29 + (int)((info >> (16 - m)) & 1u);
        if (offset < dm) {
            out->year  = 1900 + yi;
            out->month = m;
            out->day   = (int)offset + 1;
            out->leap  = false;
            return ESP_OK;
        }
        offset -= dm;

        if (m == leap_month) {
            dm = 29 + (int)((info >> 16) & 1u);
            if (offset < dm) {
                out->year  = 1900 + yi;
                out->month = m;
                out->day   = (int)offset + 1;
                out->leap  = true;
                return ESP_OK;
            }
            offset -= dm;
        }
    }

    return ESP_ERR_INVALID_STATE;
}

esp_err_t srv_lunar_format(const srv_lunar_date_t *d, char *buf, size_t cap)
{
    if (d == NULL || buf == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';

    if (d->year < 1900 || d->month < 1 || d->month > 12 ||
        d->day < 1 || d->day > 30) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 1984 年是甲子年 */
    const int idx = (d->year - 4) % 60;
    int n = snprintf(buf, cap, "%s%s年%s%s%s",
                     TIANGAN[idx % 10], DIZHI[idx % 12],
                     d->leap ? "闰" : "",
                     MONTH_NAME[d->month], DAY_NAME[d->day]);
    return (n > 0 && (size_t)n < cap) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t srv_lunar_format_solar(int year, int month, int day,
                                 char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';

    srv_lunar_date_t d;
    esp_err_t ret = srv_lunar_from_solar(year, month, day, &d);
    if (ret != ESP_OK) {
        return ret;
    }
    return srv_lunar_format(&d, buf, cap);
}
