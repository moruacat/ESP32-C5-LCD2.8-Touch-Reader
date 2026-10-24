#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 闹钟响铃回调（在闹钟任务上下文执行，勿做阻塞操作）
 * @param index 0..2 对应闹钟 1..3
 */
typedef void (*srv_alarm_cb_t)(int index, void *arg);

/**
 * @brief 初始化闹钟调度（内部 1Hz 轮询 RTC，参数取自 /config.dat）
 */
esp_err_t srv_alarm_init(void);

/**
 * @brief 注册响铃回调（可为 NULL）
 */
esp_err_t srv_alarm_register_cb(srv_alarm_cb_t cb, void *arg);

/**
 * @brief 立即检查一次（用于手动测试）
 * @return 本次触发的闹钟序号（1..3），无触发返回 0
 */
int srv_alarm_check_now(void);

/**
 * @brief 停止响铃
 * @param index 1..3
 */
esp_err_t srv_alarm_stop(int index);

/**
 * @brief 指定闹钟是否正在响铃
 */
bool srv_alarm_is_ringing(int index);

#ifdef __cplusplus
}
#endif
