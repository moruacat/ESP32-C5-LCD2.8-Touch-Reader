#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 农历（1900–2099）
 *
 * 纯计算服务，不依赖 RTC/LVGL：时钟页把公历年月日传进来，换算出农历。
 * 数据表在 lunar_table.inc（tools/gen_lunar_table.py 生成）。
 */

typedef struct {
    int  year;      /*!< 农历年（如 2026） */
    int  month;     /*!< 1–12 */
    int  day;       /*!< 1–30 */
    bool leap;      /*!< 是否闰月 */
} srv_lunar_date_t;

/**
 * @brief 公历 → 农历
 * @return ESP_OK；日期超出 1900-01-31 ~ 2099 年末时返回错误
 */
esp_err_t srv_lunar_from_solar(int year, int month, int day, srv_lunar_date_t *out);

/**
 * @brief 格式化成 "乙巳年十月十三"（闰月为 "乙巳年闰六月初一"）
 *
 * 不含「农历」前缀，调用方自己加。缓冲建议 ≥ 32 字节。
 */
esp_err_t srv_lunar_format(const srv_lunar_date_t *d, char *buf, size_t cap);

/**
 * @brief 公历直接格式化成农历字符串；失败时 buf 置空串
 */
esp_err_t srv_lunar_format_solar(int year, int month, int day,
                                 char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
