#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化局域网 OTA 服务（只起监控，真正的服务在连上 WiFi 后才开）
 *
 * WiFi 连上后自动启动 mDNS + HTTP 服务，断开后自动停止；不占着无线电。
 * 接口（详见 docs/ota_eval.md）：
 *   GET  /api/info   设备信息：版本、当前分区、能否 OTA、空闲内存
 *   POST /ota        推送固件，请求体为 clock_reader.bin，成功后会重启
 *
 * 端口固定 8080，mDNS 服务类型 _clockreader-ota._tcp。
 */
esp_err_t srv_ota_init(void);

/** @brief 当前是否已经在提供服务（已连 WiFi 且 httpd 已启动） */
bool srv_ota_is_serving(void);

/**
 * @brief 取服务地址，形如 http://192.168.1.23:8080
 * @return false 表示当前没在提供服务（或没拿到 IP）
 */
bool srv_ota_get_endpoint(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
