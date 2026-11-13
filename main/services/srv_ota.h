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

/**
 * @brief OTA 进度快照（给 UI 轮询）
 */
typedef struct {
    bool     active;      /*!< 正在接收固件 */
    bool     done;        /*!< 已写完，正在校验/重启 */
    uint32_t received;    /*!< 已接收字节 */
    uint32_t total;       /*!< 总字节，0 表示未知 */
    int      percent;     /*!< 0..100；total 未知时为 -1 */
} srv_ota_progress_t;

/**
 * @brief 读当前进度
 *
 * 没有更新在进行时 active/done 都是 false。UI 侧 200ms 轮询一次即可，
 * 这只是一次结构体拷贝，不做加锁（最坏情况是某一帧读到旧值）。
 */
void srv_ota_get_progress(srv_ota_progress_t *out);

#ifdef __cplusplus
}
#endif
