#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 NTP 服务（注册同步回调，不启动）
 */
esp_err_t srv_ntp_init(void);

/**
 * @brief 启动 NTP 校时（内部起任务等待联网后同步并写回 RTC）
 *        需 srv_config 的 ntp_en = 1 且 wifi_en = 1
 */
esp_err_t srv_ntp_autostart(void);

/**
 * @brief 停止 NTP
 */
esp_err_t srv_ntp_stop(void);

/**
 * @brief 是否已完成过一次校时
 */
bool srv_ntp_is_synced(void);

/**
 * @brief 最近一次校时的本地时间字符串（"YYYY-MM-DD HH:MM:SS"），未校时返回空串
 */
const char *srv_ntp_last_sync_text(void);

/**
 * @brief 立即校时并等待结果（阻塞）
 * @param timeout_ms 等待上限
 * @return ESP_OK 同步成功并把时间写入 RTC
 */
esp_err_t srv_ntp_sync_now(uint32_t timeout_ms);

/**
 * @brief 把当前系统时间（UTC 秒 + 时区 + 微调）写入 PCF85063A RTC
 *        供手动调时之后强制对齐，也可脱离 NTP 单独调用
 */
esp_err_t srv_ntp_write_rtc(void);

/**
 * @brief 以当前 RTC 日期重置「每日自动偏移」基准
 *
 * NTP 校时成功、手动调时、重新打开 NTP 后调用。每日偏移只在没有
 * 新 NTP 基准的期间按天累计补偿，避免重复补正。
 */
void srv_ntp_daily_reset_anchor(void);

#ifdef __cplusplus
}
#endif
