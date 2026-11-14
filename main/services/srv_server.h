#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化：从配置读服务端地址（空 = 未配置）
 *
 * 地址存在 config.dat 的 server_url 里，格式 http://ip:port。
 * 开机时如果为空，UI 会触发一次 UDP 发现。
 */
esp_err_t srv_server_init(void);

/** @brief 当前服务端地址；未配置时返回空串（不是 NULL） */
const char *srv_server_get_url(void);

/** @brief 是否已配置 */
bool srv_server_available(void);

/** @brief 手动设置地址（会落盘）。传空串等同于清除。 */
esp_err_t srv_server_set_url(const char *url);

/**
 * @brief 同步 UDP 广播发现（阻塞 timeout_ms）
 *
 * 协议：广播 "CLOCKREADER?DISCOVER=1" 到 255.255.255.255:9527，
 * 服务端回 "CLOCKREADER!URL=http://:8080;NAME=..."。
 * 回复里主机名是空的（服务端不知道自己在局域网里的 IP），
 * 这里用收到回复的源地址补上，结果写进配置。
 *
 * @return ESP_OK 发现成功；ESP_ERR_INVALID_STATE 没连 WiFi；
 *         ESP_ERR_TIMEOUT 超时；ESP_ERR_INVALID_RESPONSE 回复格式不对
 */
esp_err_t srv_server_discover(uint32_t timeout_ms);

/** @brief 异步发现（起一次性任务），UI 不必自己开任务 */
esp_err_t srv_server_auto_discover(void);

#ifdef __cplusplus
}
#endif
