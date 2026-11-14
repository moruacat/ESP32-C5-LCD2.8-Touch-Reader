#include "srv_server.h"
#include "srv_config.h"
#include "srv_wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "srv_server";

#define DISCOVER_PORT        9527
#define DISCOVER_PREFIX      "CLOCKREADER?DISCOVER=1"
#define REPLY_PREFIX         "CLOCKREADER!URL="
#define DISCOVER_TIMEOUT_MS  3000
#define DISCOVER_TASK_STACK  4096

static char s_url[SRV_CONFIG_URL_LEN];

esp_err_t srv_server_init(void)
{
    s_url[0] = '\0';
    (void)srv_config_get_str(CFG_SERVER_URL, s_url, sizeof(s_url));

    if (s_url[0] != '\0') {
        ESP_LOGI(TAG, "书库服务端：%s", s_url);
    } else {
        ESP_LOGI(TAG, "书库服务端未配置（可 UDP 发现或手动填写）");
    }
    return ESP_OK;
}

const char *srv_server_get_url(void)
{
    return s_url;
}

bool srv_server_available(void)
{
    return s_url[0] != '\0';
}

esp_err_t srv_server_set_url(const char *url)
{
    if (url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(s_url, sizeof(s_url), "%s", url);
    return srv_config_set_str(CFG_SERVER_URL, s_url, true);
}

/** 从 "http://host:port" 里拆出 host 和 port；host 可为空。 */
static void parse_url(const char *raw, char *host, size_t host_len, int *port)
{
    host[0] = '\0';
    *port = 8080;

    const char *scheme = strstr(raw, "://");
    const char *hp = scheme ? scheme + 3 : raw;
    const char *colon = strchr(hp, ':');

    if (colon != NULL) {
        size_t hl = (size_t)(colon - hp);
        if (hl < host_len) {
            memcpy(host, hp, hl);
            host[hl] = '\0';
        }
        if (colon[1] != '\0') {
            *port = atoi(colon + 1);
        }
    } else if (hp[0] != '\0') {
        snprintf(host, host_len, "%s", hp);
    }
}

esp_err_t srv_server_discover(uint32_t timeout_ms)
{
    if (!srv_wifi_is_connected()) {
        ESP_LOGW(TAG, "未连 WiFi，无法发现服务端");
        return ESP_ERR_INVALID_STATE;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "创建 UDP socket 失败");
        return ESP_FAIL;
    }

    int bcast = 1;
    (void)setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

    struct timeval tv = {
        .tv_sec  = (time_t)(timeout_ms / 1000),
        .tv_usec = (suseconds_t)((timeout_ms % 1000) * 1000),
    };
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family      = AF_INET;
    dest.sin_port        = htons(DISCOVER_PORT);
    dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    if (sendto(sock, DISCOVER_PREFIX, strlen(DISCOVER_PREFIX), 0,
               (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        ESP_LOGE(TAG, "发送发现广播失败");
        close(sock);
        return ESP_FAIL;
    }

    char reply[256];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    int n = recvfrom(sock, reply, sizeof(reply) - 1, 0,
                     (struct sockaddr *)&from, &from_len);
    close(sock);

    if (n <= 0) {
        ESP_LOGW(TAG, "UDP 发现超时（%u ms）", (unsigned)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    reply[n] = '\0';

    const char *p = strstr(reply, REPLY_PREFIX);
    if (p == NULL) {
        ESP_LOGW(TAG, "回复格式不对：%.64s", reply);
        return ESP_ERR_INVALID_RESPONSE;
    }
    p += strlen(REPLY_PREFIX);

    // URL 以 ';' 结束（后面是 NAME=...）
    char raw[96];
    size_t len = strcspn(p, ";");
    if (len >= sizeof(raw)) {
        len = sizeof(raw) - 1;
    }
    memcpy(raw, p, len);
    raw[len] = '\0';

    char host[48];
    int  port = 8080;
    parse_url(raw, host, sizeof(host), &port);

    // 回复里的 host 通常是空的（服务端不知道自己的局域网 IP），
    // 用「收到回复的源地址」补上——这样无论服务端有几块网卡都能连对。
    if (host[0] == '\0') {
        snprintf(host, sizeof(host), "%s", inet_ntoa(from.sin_addr));
    }

    char url[SRV_CONFIG_URL_LEN];
    snprintf(url, sizeof(url), "http://%s:%d", host, port);
    (void)srv_server_set_url(url);

    ESP_LOGI(TAG, "发现书库服务端：%s", url);
    return ESP_OK;
}

static void discover_task(void *arg)
{
    (void)arg;
    (void)srv_server_discover(DISCOVER_TIMEOUT_MS);
    vTaskDelete(NULL);
}

esp_err_t srv_server_auto_discover(void)
{
    if (!srv_wifi_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(discover_task, "srv_disc", DISCOVER_TASK_STACK, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "发现任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
