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

/* ===================== 下载 ===================== */

/* 定义在文件末尾（与 srv_catalog_state 放在一起），这里先声明：
 * 下载流程要用它判断「本地已有几章」来决定全量还是增量。 */
static int local_chapters(const srv_catalog_book_t *book);

#define DOWNLOAD_BUF   4096
#define META_MAX_BYTES (2 * 1024 * 1024)   /* data/chapters/cover 各自的硬上限 */

/** 书名 → 目录名：替换 FATFS 不接受的字符，与 Python/Go 的规则近似。 */
static void safe_name(const char *title, char *out, size_t out_len)
{
    size_t o = 0;
    for (const char *p = title; *p != '\0' && o + 1 < out_len; p++) {
        const char c = *p;
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || c < 0x20) {
            out[o++] = '_';
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
    if (o == 0) {
        snprintf(out, out_len, "book");
    }
}

/** 发起 GET 并把响应流式写进 path；max_bytes 用来挡住异常大的响应。 */
static esp_err_t http_get_to_file(const char *url, const char *path, size_t max_bytes)
{
    esp_http_client_config_t cfg = {
        .url         = url,
        .timeout_ms  = HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
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

    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "打不开 %s 写", path);
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        return ESP_FAIL;
    }

    char *buf = malloc(DOWNLOAD_BUF);
    if (buf == NULL) {
        fclose(fp);
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    esp_err_t result = ESP_OK;
    for (;;) {
        int n = esp_http_client_read(cli, buf, DOWNLOAD_BUF);
        if (n < 0) {
            ESP_LOGE(TAG, "读响应失败：%s", url);
            result = ESP_FAIL;
            break;
        }
        if (n == 0) {
            break;
        }
        total += (size_t)n;
        if (total > max_bytes) {
            ESP_LOGE(TAG, "响应超过 %u 字节，放弃：%s",
                     (unsigned)max_bytes, url);
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (fwrite(buf, 1, (size_t)n, fp) != (size_t)n) {
            ESP_LOGE(TAG, "写 %s 失败（SD 满了？）", path);
            result = ESP_FAIL;
            break;
        }
    }

    free(buf);
    fclose(fp);
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);

    if (result != ESP_OK) {
        (void)srv_fs_remove(path);      /* 别留半个文件 */
    }
    return result;
}

esp_err_t srv_catalog_download(const srv_catalog_book_t *book,
                               srv_catalog_progress_cb_t progress_cb)
{
    if (book == NULL || book->id <= 0 || book->chapters <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!srv_sd_is_mounted()) {
        ESP_LOGE(TAG, "SD 未挂载");
        return ESP_ERR_INVALID_STATE;
    }

    const char *base = srv_server_get_url();
    if (base == NULL || base[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    /* ---- 目标目录 /books/<书名>.ebf ---- */
    char name[SRV_CATALOG_NAME_MAX];
    safe_name(book->title, name, sizeof(name));

    char books_dir[64];
    snprintf(books_dir, sizeof(books_dir), "%s/books", srv_sd_mount_point());

    char dir[96];
    snprintf(dir, sizeof(dir), "%s/%s.ebf", books_dir, name);

    /* 已经下过前 N 章、服务端又加了新章时，只补新增部分 ——
     * 追更一本书不该把前面几百章重下一遍（也免得覆盖掉阅读进度）。 */
    const int local = local_chapters(book);
    const bool incremental = (local > 0 && local < book->chapters);
    const int from = incremental ? (local + 1) : 1;

    if (!incremental) {
        /* 全新下载，或本地章节数比服务端多（书被换过）→ 清掉重来 */
        (void)srv_fs_remove_tree(dir);
    }
    (void)srv_fs_mkdir(books_dir);      /* 已存在会返回错误，无妨 */
    esp_err_t err = srv_fs_mkdir(dir);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "建目录失败：%s", dir);
        return err;
    }

    if (incremental) {
        ESP_LOGI(TAG, "%s：本地已有 %d 章，只补 %d-%d 章",
                 book->title, local, from, book->chapters);
    }

    char url[192];
    char path[128];

    /* ---- 先下正文章节，最后才更新元数据 ----
     * 顺序很重要：data.ebfl 里的 chapters 是固件判断"这本书有多少章"的依据。
     * 如果先更新它、章节却没下完，书架会显示 N 章但点进去缺文件。
     * 放到最后，中途失败时本地的旧 data.ebfl 仍然和旧章节自洽。 */
    for (int i = from; i <= book->chapters; i++) {
        snprintf(url, sizeof(url), "%s/api/books/%lld/chapter/%d",
                 base, (long long)book->id, i);
        snprintf(path, sizeof(path), "%s/chapter.%d.ebfl", dir, i);

        err = http_get_to_file(url, path, META_MAX_BYTES);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "第 %d/%d 章下载失败", i, book->chapters);
            goto fail;
        }

        if (progress_cb != NULL) {
            progress_cb(i, book->chapters);
        }
    }

    /* ---- 元数据三件套（章节全部就绪后再写） ---- */
    snprintf(url, sizeof(url), "%s/api/books/%lld/data", base, (long long)book->id);
    snprintf(path, sizeof(path), "%s/data.ebfl", dir);
    if (http_get_to_file(url, path, META_MAX_BYTES) != ESP_OK) {
        goto fail;
    }

    snprintf(url, sizeof(url), "%s/api/books/%lld/chapters", base, (long long)book->id);
    snprintf(path, sizeof(path), "%s/chapters.ebfl", dir);
    if (http_get_to_file(url, path, META_MAX_BYTES) != ESP_OK) {
        goto fail;
    }

    // 封面可能没有（服务端没解析到封面），失败不算致命
    snprintf(url, sizeof(url), "%s/api/books/%lld/cover", base, (long long)book->id);
    snprintf(path, sizeof(path), "%s/cover.raw", dir);
    if (http_get_to_file(url, path, META_MAX_BYTES) != ESP_OK) {
        ESP_LOGW(TAG, "封面下载失败，继续（不影响阅读）");
        (void)srv_fs_remove(path);
    }

    ESP_LOGI(TAG, "下载完成：%s（%d 章%s）", book->title, book->chapters,
             incremental ? "，增量补章" : "");
    return ESP_OK;

fail:
    /* 全新下载失败：不留半本书（书架会扫出缺章的书）。
     * 增量失败：保留旧章节和旧 data.ebfl，它们仍然自洽，下次重试即可。 */
    if (!incremental) {
        (void)srv_fs_remove_tree(dir);
    }
    return err != ESP_OK ? err : ESP_FAIL;
}

/** 读本地 data.ebfl 里的章节数；0 表示没下载或读不出。 */
static int local_chapters(const srv_catalog_book_t *book)
{
    if (book == NULL || !srv_sd_is_mounted()) {
        return 0;
    }

    char name[SRV_CATALOG_NAME_MAX];
    safe_name(book->title, name, sizeof(name));

    char path[96];
    snprintf(path, sizeof(path), "%s/books/%s.ebf/data.ebfl",
             srv_sd_mount_point(), name);

    srv_fs_entry_t st;
    if (srv_fs_stat(path, &st) != ESP_OK) {
        return 0;
    }

    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return 0;
    }
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';

    const char *p = strstr(buf, "chapters=\"");
    if (p == NULL) {
        return 0;
    }
    p += strlen("chapters=\"");
    return atoi(p);
}

srv_catalog_state_t srv_catalog_state(const srv_catalog_book_t *book)
{
    const int local = local_chapters(book);
    if (local <= 0) {
        return SRV_CATALOG_NONE;
    }
    return (local == book->chapters) ? SRV_CATALOG_UP_TO_DATE
                                     : SRV_CATALOG_OUTDATED;
}
