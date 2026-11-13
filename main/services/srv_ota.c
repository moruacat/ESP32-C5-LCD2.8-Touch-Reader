#include "srv_ota.h"
#include "srv_wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"

static const char *TAG = "srv_ota";

#define OTA_HTTP_PORT   8080
#define OTA_RECV_BUF    4096
#define OTA_MONITOR_MS  2000
/* 监控任务只是每 2 秒查一次 WiFi 状态，2KB 栈绰绰有余；
 * 任务栈占的是内部 RAM，省下来留给小智 AI。 */
#define OTA_TASK_STACK  2048

static httpd_handle_t s_server = NULL;
static bool           s_mdns_up = false;
static volatile bool  s_ota_busy = false;
/*! 进度快照：httpd 任务写、UI 任务读。只做结构体拷贝，不加锁 ——
 *  最坏情况是某一帧读到旧百分比，比在 hot path 里抢锁划算。 */
static srv_ota_progress_t s_prog;

/* ===================== HTTP 接口 ===================== */

static esp_err_t info_get(httpd_req_t *req)
{
    const esp_app_desc_t   *desc    = esp_app_get_description();
    const esp_partition_t  *running = esp_ota_get_running_partition();
    const esp_partition_t  *update  = esp_ota_get_next_update_partition(NULL);

    char body[320];
    int n = snprintf(body, sizeof(body),
                     "{\"name\":\"ClockReader\",\"version\":\"%s\","
                     "\"built\":\"%s %s\",\"partition\":\"%s\","
                     "\"ota_ready\":%s,\"free_heap\":%u}",
                     desc ? desc->version : "?", desc ? desc->date : "?",
                     desc ? desc->time : "?",
                     running ? running->label : "?",
                     update ? "true" : "false",
                     (unsigned)esp_get_free_heap_size());
    if (n < 0 || n >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "info too long");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (s_ota_busy) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "OTA already running");
        return ESP_FAIL;
    }

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (update == NULL) {
        ESP_LOGE(TAG, "没有可写的 OTA 分区：分区表还是单 app（见 docs/ota_eval.md）");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ota partition");
        return ESP_FAIL;
    }

    /* 目标分区 == 当前运行分区，说明分区表里只有一个 ota 分区（通常是
     * factory + ota_0 这种）。esp_ota_begin 会报 ESP_ERR_OTA_PARTITION_CONFLICT，
     * 这里提前拦下来，把原因说清楚，免得只看到一个错误码。 */
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (update == running) {
        ESP_LOGE(TAG, "目标分区就是当前运行分区（%s）：分区表缺少 ota_1，"
                      "OTA 无法轮换，需要在 partitions.csv 里补一个 ota_1 分区",
                 update->label);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no alternated ota partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "开始 OTA：写入 %s，声明长度 %u 字节",
             update->label, (unsigned)req->content_len);

    uint8_t *buf = malloc(OTA_RECV_BUF);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_FAIL;
    }

    esp_ota_handle_t handle = 0;
    /* 用 Content-Length 当镜像大小：esp_ota_begin 只擦这么大。
     * 传 OTA_SIZE_UNKNOWN 会把整个 6MB 分区都擦掉，白花十几秒，
     * 期间 flash cache 长时间关闭，WiFi 更容易掉。 */
    size_t image_size = (req->content_len > 0) ? (size_t)req->content_len
                                               : (size_t)OTA_SIZE_UNKNOWN;
    esp_err_t err = esp_ota_begin(update, image_size, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin 失败：%s", esp_err_to_name(err));
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
        return ESP_FAIL;
    }

    s_ota_busy = true;
    s_prog.active   = true;
    s_prog.done     = false;
    s_prog.received = 0;
    s_prog.total    = (uint32_t)req->content_len;
    s_prog.percent  = 0;

    int remaining = req->content_len;
    int total = 0;
    while (remaining > 0) {
        int want = remaining < OTA_RECV_BUF ? remaining : OTA_RECV_BUF;
        int n = httpd_req_recv(req, (char *)buf, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;                       /* 慢网络，继续等这一块 */
        }
        if (n <= 0) {
            ESP_LOGE(TAG, "接收中断，已收 %d 字节", total);
            esp_ota_abort(handle);
            free(buf);
            s_ota_busy = false;
            s_prog.active = false;
            s_prog.percent = -1;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        err = esp_ota_write(handle, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write 失败：%s", esp_err_to_name(err));
            esp_ota_abort(handle);
            free(buf);
            s_ota_busy = false;
            s_prog.active = false;
            s_prog.percent = -1;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
            return ESP_FAIL;
        }
        total += n;
        remaining -= n;
        s_prog.received = (uint32_t)total;
        s_prog.percent  = (req->content_len > 0)
                          ? (int)((int64_t)total * 100 / req->content_len) : -1;
    }

    free(buf);
    s_ota_busy = false;

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        /* 最常见的是传上来的不是完整 app 镜像（比如 .zip 或 bootloader） */
        ESP_LOGE(TAG, "esp_ota_end 失败（镜像校验不过）：%s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "image invalid");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition 失败：%s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set boot failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA 完成：%d 字节已写入 %s，重启后生效", total, update->label);
    s_prog.active  = false;
    s_prog.done    = true;
    s_prog.percent = 100;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", 11);

    vTaskDelay(pdMS_TO_TICKS(800));          /* 留时间把响应发出去 */
    esp_restart();
    return ESP_OK;
}

/* ===================== 服务启停 ===================== */

static esp_err_t serving_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port    = OTA_HTTP_PORT;
    cfg.max_open_sockets = 2;
    /* esp_ota_write + flash 擦写在 httpd 任务栈里跑。8KB 够用：
     * 之前设 12KB 是为「保险」，但这几个任务栈都在**内部 RAM**，
     * 而小智 AI 启动时就是因为内部 RAM 吃紧（最低只剩 3KB）建不出任务。 */
    cfg.stack_size     = 8192;
    /* 擦除大块时客户端会被 TCP 窗口堵住，给接收多留点余量 */
    cfg.recv_wait_timeout = 10;
    cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd 启动失败：%s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    const httpd_uri_t info_uri = {
        .uri = "/api/info", .method = HTTP_GET, .handler = info_get,
    };
    const httpd_uri_t ota_uri = {
        .uri = "/ota", .method = HTTP_POST, .handler = ota_post,
    };
    httpd_register_uri_handler(s_server, &info_uri);
    httpd_register_uri_handler(s_server, &ota_uri);

    if (!s_mdns_up) {
        if (mdns_init() == ESP_OK) {
            const esp_app_desc_t *desc = esp_app_get_description();
            mdns_txt_item_t txt[1] = {
                {"version", (char *)(desc ? desc->version : "unknown")},
            };
            mdns_hostname_set("clockreader");
            mdns_instance_name_set("ClockReader");
            mdns_service_add("ClockReader OTA", "_clockreader-ota", "_tcp",
                             OTA_HTTP_PORT, txt, 1);
            s_mdns_up = true;
        } else {
            ESP_LOGW(TAG, "mDNS 初始化失败：CI 工具将无法自动发现设备，仍需手动填 IP");
        }
    }

    char ip[16] = {0};
    (void)srv_wifi_get_ip(ip, sizeof(ip));
    ESP_LOGI(TAG, "OTA 服务已启动：http://%s:%d（clockreader.local）",
             ip[0] ? ip : "?", OTA_HTTP_PORT);
    return ESP_OK;
}

static void serving_stop(void)
{
    if (s_ota_busy) {
        /* 正在收固件时绝不能关服务：httpd_stop() 会把当前 socket 一起关掉，
         * 客户端看到的就是「远程主机强迫关闭了一个现有的连接」(10054)。
         * 擦除大分区时 flash cache 关闭，WiFi 可能短暂报断连，监控任务
         * 正好会撞上这里 —— 放它过去，等这一轮写完自然结束。 */
        ESP_LOGW(TAG, "OTA 进行中，暂不停止服务");
        return;
    }
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "OTA 服务已停止");
    }
    if (s_mdns_up) {
        mdns_free();
        s_mdns_up = false;
    }
}

static void ota_monitor_task(void *arg)
{
    (void)arg;
    bool last = false;
    for (;;) {
        bool connected = srv_wifi_is_connected();
        if (connected && !last) {
            serving_start();
        } else if (!connected && last) {
            serving_stop();
        }
        last = connected;
        vTaskDelay(pdMS_TO_TICKS(OTA_MONITOR_MS));
    }
}

/* ===================== 对外接口 ===================== */

esp_err_t srv_ota_init(void)
{
    static bool started = false;
    if (started) {
        return ESP_OK;
    }
    if (xTaskCreate(ota_monitor_task, "ota_mon", OTA_TASK_STACK, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "OTA 监控任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    started = true;
    ESP_LOGI(TAG, "OTA 服务就绪（WiFi 连上后自动启用，端口 %d）", OTA_HTTP_PORT);
    return ESP_OK;
}

bool srv_ota_is_serving(void)
{
    return s_server != NULL;
}

bool srv_ota_get_endpoint(char *out, size_t out_len)
{
    if (s_server == NULL || out == NULL || out_len == 0) {
        return false;
    }
    char ip[16] = {0};
    if (srv_wifi_get_ip(ip, sizeof(ip)) != ESP_OK || ip[0] == '\0') {
        return false;
    }
    snprintf(out, out_len, "http://%s:%d", ip, OTA_HTTP_PORT);
    return true;
}

void srv_ota_get_progress(srv_ota_progress_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_prog;
}
