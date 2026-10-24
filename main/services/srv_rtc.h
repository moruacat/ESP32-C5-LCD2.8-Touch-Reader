#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief RTC 时间结构
 */
typedef struct {
    int year;
    int month;
    int day;
    int dotw;   /*!< 星期，0=周日 */
    int hour;
    int min;
    int sec;
} srv_rtc_time_t;

/**
 * @brief 初始化 PCF85063A（不写入固定时间，仅在时间无效时设置默认值）
 */
esp_err_t srv_rtc_init(void);

/**
 * @brief 读取当前时间
 */
esp_err_t srv_rtc_get_time(srv_rtc_time_t *t);

/**
 * @brief 设置时间（NTP 校时等场景使用）
 */
esp_err_t srv_rtc_set_time(const srv_rtc_time_t *t);

/**
 * @brief 由年月日推算星期（0=周日，Sakamoto 算法）
 *        手动调时、NTP 校时都需要它补 dotw 字段
 */
int srv_rtc_calc_dotw(int year, int month, int day);

/**
 * @brief RTC 时间是否有效（年份合理）
 */
bool srv_rtc_time_valid(const srv_rtc_time_t *t);

#ifdef __cplusplus
}
#endif
