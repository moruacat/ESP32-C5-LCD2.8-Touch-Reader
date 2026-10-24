#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化板载传感器（QMI8658 六轴、PCF85063A RTC、SHTC3 温湿度）
 *        单个传感器失败不阻塞整体
 */
esp_err_t srv_sensors_init(void);

/**
 * @brief 读取温湿度（摄氏度 / %RH）
 */
esp_err_t srv_sensors_get_temp_humi(float *temp_c, float *humi_pct);

/* ===================== 温湿度记录 ===================== */

/**
 * @brief 启动周期记录：按 interval_min 分钟采样一次，追加到当天 CSV
 *
 * 存储结构（按天分文件）：
 *   <SD 挂载点>/temp-rec/YYYY/MM/DD.csv
 * 例：/sdcard/temp-rec/2026/09/19.csv
 *
 * 文件格式（同一天不重复写日期）：
 *   # 时间,温度(℃),湿度(%RH)
 *   09:30:00,26.3,48.1
 *
 * 目录不存在会自动逐级创建；RTC 时间无效时跳过本次记录。
 *
 * @param interval_min 采样间隔（分钟），<= 0 时取默认 10 分钟
 */
esp_err_t srv_sensors_record_start(int interval_min);

/**
 * @brief 停止周期记录
 */
esp_err_t srv_sensors_record_stop(void);

/**
 * @brief 是否正在记录
 */
bool srv_sensors_record_running(void);

/**
 * @brief 立即采样并写一行
 * @return ESP_OK 写入成功
 */
esp_err_t srv_sensors_record_now(void);

/**
 * @brief 取「今天」的记录文件路径（<SD>/temp-rec/YYYY/MM/DD.csv）
 * @param out     输出缓冲
 * @param out_len 缓冲长度
 * @return ESP_OK；RTC 时间无效时返回 ESP_ERR_INVALID_STATE 并置空串
 */
esp_err_t srv_sensors_record_path(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
