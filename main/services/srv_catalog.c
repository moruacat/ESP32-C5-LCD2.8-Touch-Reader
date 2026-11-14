#include "srv_catalog.h"
#include "srv_server.h"
#include "srv_fs.h"
#include "srv_sd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "srv_catalog";

#define HTTP_TIMEOUT_MS   10000
#define BODY_INIT_BYTES   (16 * 1024)
#define BODY_MAX_BYTES    (256 * 1024)   /* 书单最多 64 本，256KB 足够 */

/* ===================== HTTP 小工具 ===================== */

/** 发起一次 GET 并把整个响应体读进 *body（malloc 分配，调用方 free）。 */
static esp_err_t http_get_body(const char *url, char **body, int *body_len)
{
    *body = NULL;
    *body_len = 0;

    esp_http_client_config_t cfg = {
        .url        = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "连接失败：%s", esp_err_to_name(err));
        esp_http_client_cleanup(cli);
        return err;
    }

    (void)esp_http_client_fetch_headers(cli);
    const int status = esp_http_client_get_status_code(cli);
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d：%s", status, url);
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        return ESP_FAIL;
    }

    int cap = BODY_INIT_BYTES;
    char *buf = malloc(cap);
    if (buf == NULL) {
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        return ESP_ERR_NO_MEM;
    }

    int len = 0;
    for (;;) {
        if (len + 1 >= cap) {
            if (cap >= BODY_MAX_BYTES) {
                ESP_LOGE(TAG, "响应超过 %d 字节，放弃", BODY_MAX_BYTES);
                free(buf);
                esp_http_client_close(cli);
                esp_http_client_cleanup(cli);
                return ESP_ERR_NO_MEM;
            }
            int ncap = cap * 2;
            char *nb = realloc(buf, ncap);
            if (nb == NULL) {
                free(buf);
                esp_http_client_close(cli);
                esp_http_client_cleanup(cli);
                return ESP_ERR_NO_MEM;
            }
            buf = nb;
            cap = ncap;
        }

        int n = esp_http_client_read(cli, buf + len, cap - len - 1);
        if (n < 0) {
            ESP_LOGE(TAG, "读响应失败");
            free(buf);
            esp_http_client_close(cli);
            esp_http_client_cleanup(cli);
            return ESP_FAIL;
        }
        if (n == 0) {
            break;
        }
        len += n;
    }
    buf[len] = '\0';

    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);

    *body = buf;
    *body_len = len;
    return ESP_OK;
}

/* ===================== 书单 ===================== */

static void copy_str(char *dst, size_t dst_len, const cJSON *item, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(item, key);
    if (cJSON_IsString(v) && v->valuestring != NULL) {
        snprintf(dst, dst_len, "%s", v->valuestring);
    } else {
        dst[0] = '\0';
    }
}

esp_err_t srv_catalog_fetch(srv_catalog_book_t **out, int *count)
{
    *out = NULL;
    *count = 0;

    const char *base = srv_server_get_url();
    if (base == NULL || base[0] == '\0') {
        ESP_LOGW(TAG, "还没配置服务端地址");
        return ESP_ERR_INVALID_STATE;
    }

    char url[160];
    snprintf(url, sizeof(url), "%s/api/books", base);

    char *body = NULL;
    int len = 0;
    esp_err_t err = http_get_body(url, &body, &len);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        ESP_LOGE(TAG, "书单 JSON 解析失败");
        return ESP_FAIL;
    }

    const cJSON *books = cJSON_GetObjectItemCaseSensitive(root, "books");
    if (!cJSON_IsArray(books)) {
        ESP_LOGW(TAG, "书单里没有 books 数组");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    int n = cJSON_GetArraySize(books);
    if (n > SRV_CATALOG_MAX) {
        n = SRV_CATALOG_MAX;
    }
    if (n <= 0) {
        cJSON_Delete(root);
        return ESP_OK;
    }

    srv_catalog_book_t *list = calloc((size_t)n, sizeof(*list));
    if (list == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    int filled = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, books) {
        if (filled >= n) {
            break;
        }
        srv_catalog_book_t *b = &list[filled];

        const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
        b->id = cJSON_IsNumber(id) ? (int64_t)id->valuedouble : 0;
        copy_str(b->title, sizeof(b->title), item, "title");
        copy_str(b->author, sizeof(b->author), item, "author");
        copy_str(b->hash, sizeof(b->hash), item, "hash");

        const cJSON *ch = cJSON_GetObjectItemCaseSensitive(item, "chapters");
        b->chapters = cJSON_IsNumber(ch) ? ch->valueint : 0;

        const cJSON *by = cJSON_GetObjectItemCaseSensitive(item, "bytes");
        b->bytes = cJSON_IsNumber(by) ? (int64_t)by->valuedouble : 0;

        if (b->id > 0) {
            filled++;
        }
    }

    cJSON_Delete(root);

    if (filled == 0) {
        free(list);
        return ESP_OK;
    }

    *out = list;
    *count = filled;
    ESP_LOGI(TAG, "书单：%d 本", filled);
    return ESP_OK;
}

void srv_catalog_free(srv_catalog_book_t *list)
{
    free(list);
}

/* ===================== 下载（下一阶段实现） ===================== */

esp_err_t srv_catalog_download(const srv_catalog_book_t *book,
                               srv_catalog_progress_cb_t progress_cb)
{
    (void)book;
    (void)progress_cb;
    ESP_LOGW(TAG, "分章下载尚未实现");
    return ESP_ERR_NOT_SUPPORTED;
}
