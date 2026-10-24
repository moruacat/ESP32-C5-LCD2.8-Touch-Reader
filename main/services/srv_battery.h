#pragma once
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化电池监测（读取 CH32 ADC）
 */
esp_err_t srv_battery_init(void);

/**
 * @brief 获取电池电压（V）与电量百分比（0-100）
 * @return ESP_OK 读取成功
 */
esp_err_t srv_battery_get(float *voltage, int *percent);

/**
 * @brief 读 CH32 的原始 ADC 值（0~1023），用于标定电压换算
 */
esp_err_t srv_battery_get_adc(uint16_t *adc);

/* ===================== 校准 ===================== */

/**
 * @brief 当前电压修正系数（千分比，1000 = 未校准/按板上 3:1 原样换算）
 */
int srv_battery_cal_k(void);

/**
 * @brief 单点校准：拿万用表量到的真实电压算修正系数并存进配置
 *
 * 板上分压电阻、ADC 参考都有误差，±5% 很常见 —— 量一次真实电压就能校回来。
 * 系数 = 实测电压 / 未修正电压，之后所有读数都乘它。
 *
 * @param measured_v 万用表实测电压（V），合理范围 2.0~5.0
 * @return ESP_OK / ESP_ERR_INVALID_ARG（超范围）/ ESP_ERR_INVALID_STATE（没装电池）
 */
esp_err_t srv_battery_calibrate(float measured_v);

/*! 恢复默认系数（1000‰） */
void srv_battery_cal_reset(void);

#ifdef __cplusplus
}
#endif