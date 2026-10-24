#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SRV_WIFI_SSID_MAX     33      /*!< SSID 缓冲（含结束符） */
#define SRV_WIFI_PASS_MAX     65      /*!< 密码缓冲 */
#define SRV_WIFI_HISTORY_MAX  8       /*!< 网络历史条数上限 */

/**
 * @brief 扫描到的热点
 */
typedef struct {
    char    ssid[SRV_WIFI_SSID_MAX];
    int8_t  rssi;                     /*!< 信号强度 dBm */
    uint8_t authmode;                 /*!< wifi_auth_mode_t，0=开放 */
} srv_wifi_ap_t;

/* ===================== 生命周期 ===================== */

/**
 * @brief 初始化 WiFi（netif + 事件组 + STA 模式），并加载网络历史
 *        不自动开启射频；是否联网由 srv_config 的 wifi_en 决定
 */
esp_err_t srv_wifi_init(void);

/**
 * @brief 网络开关：开 = 启动射频并按历史自动连接；关 = 断连并关闭射频
 */
esp_err_t srv_wifi_set_enabled(bool on);

bool srv_wifi_is_enabled(void);

/* ===================== 状态 ===================== */

bool srv_wifi_is_connected(void);

/**
 * @brief 取当前 IPv4 地址字符串
 */
esp_err_t srv_wifi_get_ip(char *out, size_t out_len);

int8_t srv_wifi_get_rssi(void);

/**
 * @brief 当前连接的 SSID（未连接返回空串）
 */
const char *srv_wifi_get_ssid(void);

/* ===================== 扫描与连接 ===================== */

/**
 * @brief 同步扫描（阻塞，约 2~6 秒）
 * @param cb       每个热点回调一次，可为 NULL
 * @param arg      透传
 * @param ap_count 输出热点数量，可为 NULL
 */
esp_err_t srv_wifi_scan(void (*cb)(const srv_wifi_ap_t *ap, void *arg), void *arg, int *ap_count);

/**
 * @brief 连接指定网络
 *
 * @param save   true 时同时写入 srv_config 与网络历史
 * @param manual true = 用户在界面上手动选的网络：会先断开当前连接再连，
 *               并叫停后台的自动连接（否则它会在连上之后断开去试下一条）
 */
esp_err_t srv_wifi_connect(const char *ssid, const char *pass, bool save, bool manual);

esp_err_t srv_wifi_disconnect(void);

/**
 * @brief 是否正在连接/重连
 *
 * 扫描和连接会互相打断（esp_wifi_scan_start 在连接过程中会直接失败），
 * 界面进扫描页前可以据此先等一会儿。
 */
bool srv_wifi_is_connecting(void);

/**
 * @brief 开机自动连接：扫描 → 按历史顺序匹配 → 逐个尝试
 *        异步执行（内部起一次性任务），结果通过状态接口查询
 */
esp_err_t srv_wifi_autoconnect(void);

/* ===================== 网络历史（/sdcard/wifi.dat） ===================== */

/**
 * @brief 历史条数
 */
int srv_wifi_history_count(void);

/**
 * @brief 按序号取历史记录（0 = 最近一次使用的）
 * @return ESP_OK / ESP_ERR_INVALID_ARG
 */
esp_err_t srv_wifi_history_at(int idx, char *ssid, size_t ssid_len, char *pass, size_t pass_len);

/**
 * @brief 按 SSID 查密码（自动连接用）
 */
esp_err_t srv_wifi_history_get(const char *ssid, char *pass, size_t pass_len);

/**
 * @brief 新增或更新一条历史（同时落盘），已有的会被提到最前
 */
esp_err_t srv_wifi_history_add(const char *ssid, const char *pass);

/**
 * @brief 删除一条历史并落盘
 */
esp_err_t srv_wifi_history_remove(const char *ssid);

/**
 * @brief 清空历史并落盘
 */
esp_err_t srv_wifi_history_clear(void);

#ifdef __cplusplus
}
#endif
