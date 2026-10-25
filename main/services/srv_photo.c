#include "srv_photo.h"
#include "srv_sd.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "srv_photo";

#define PHOTO_DIR_NAME  "photos"
#define RAW_SUFFIX      ".raw"

/* 收集容量，比显示上限大：目录给的是字典序，若收到 SRV_PHOTO_MAX 就停，
 * 留下的是「字典序前 N 个」（1、10、100…），排完序仍是一堆断号。
 * 所以先按这个容量全收进来，排序后再截断成自然序最小的那批。 */
#define PHOTO_HARD_MAX  1024

static char        s_root[64];
static srv_photo_t *s_items = NULL;
static int          s_count = 0;
static bool         s_overflow = false;     /*!< 本轮扫描是否超限（只报一次日志） */
static int          s_bad = 0;              /*!< 本轮扫描剔掉的大小异常文件数 */
/*! 扫描标志：扫描期间条目表在被改写，查表/读图先让开 */
static volatile bool s_busy = false;

/* ===================== 路径 ===================== */

const char *srv_photo_root(void)
{
    if (s_root[0] == '\0') {
        snprintf(s_root, sizeof(s_root), "%s/%s", srv_sd_mount_point(), PHOTO_DIR_NAME);
    }
    return s_root;
}

/* ===================== 内存 ===================== */

static esp_err_t items_alloc(void)
{
    if (s_items != NULL) {
        return ESP_OK;
    }

    const size_t bytes = sizeof(srv_photo_t) * PHOTO_HARD_MAX;
    /* 条目常驻内存（约 0.6MB @ 1024 张），优先放 PSRAM */
    s_items = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_items == NULL) {
        ESP_LOGW(TAG, "PSRAM 分配失败，改用内部 RAM");
        s_items = malloc(bytes);
    }
    if (s_items == NULL) {
        ESP_LOGE(TAG, "照片表内存分配失败（%u 字节）", (unsigned)bytes);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ===================== 扫描 ===================== */

/* 大小写不敏感地判断是否以 suffix 结尾 */
static bool ends_with_ci(const char *s, const char *suffix)
{
    size_t ls = strlen(s);
    size_t lf = strlen(suffix);
    if (ls < lf) {
        return false;
    }
    return strcasecmp(s + ls - lf, suffix) == 0;
}

/* 自然序比较：数字段按数值比（2 < 10 < 100），其余按字符（大小写不敏感）。
 * 返回负/零/正，语义同 strcmp/qsort 的 cmp。 */
static int nat_cmp(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        /* 两边都是数字：把整段数字按数值比，而不是逐位字典序 */
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            char *ea = NULL;
            char *eb = NULL;
            unsigned long va = strtoul(a, &ea, 10);
            unsigned long vb = strtoul(b, &eb, 10);
            if (va < vb) {
                return -1;
            }
            if (va > vb) {
                return 1;
            }
            /* 数值相等（如 "01" vs "1"）就跳过这一段继续比后面 */
            a = ea;
            b = eb;
            continue;
        }
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) {
            return (ca < cb) ? -1 : 1;
        }
        a++;
        b++;
    }
    if (*a == '\0' && *b == '\0') {
        return 0;
    }
    return (*a == '\0') ? -1 : 1;
}

static int photo_cmp(const void *x, const void *y)
{
    const srv_photo_t *pa = (const srv_photo_t *)x;
    const srv_photo_t *pb = (const srv_photo_t *)y;
    return nat_cmp(pa->name, pb->name);
}

static bool add_from_entry(const srv_fs_entry_t *e, void *arg)
{
    (void)arg;

    if (e->is_dir || !ends_with_ci(e->name, RAW_SUFFIX)) {
        return true;                    /* 只要 .raw 文件，其余一律跳过 */
    }
    /* 尺寸不对的（没拷完、被截断、或不是 photo2raw 转的）直接在扫描阶段剔掉：
     * 列表里不该出现一张点开就报"文件损坏"的条目。 */
    if (e->size != SRV_PHOTO_BYTES) {
        s_bad++;
        return true;
    }
    if (s_count >= PHOTO_HARD_MAX) {
        /* 到这里说明照片比 PHOTO_HARD_MAX 还多（>1024 张）：
         * 继续收也没意义，且收满的已经是字典序前一批，排完序照样断号。
         * 停在收集阶段，末尾统一提示。 */
        if (!s_overflow) {
            s_overflow = true;
            ESP_LOGW(TAG, "照片超过 %d 张，多余的忽略（只显示前 %d 张）",
                     PHOTO_HARD_MAX, SRV_PHOTO_MAX);
        }
        return true;
    }

    srv_photo_t *p = &s_items[s_count];
    memset(p, 0, sizeof(*p));
    /* 用 %.*s 显式限长：entry.name 比缓冲长，直接 %s 会触发 -Wformat-truncation */
    snprintf(p->name, sizeof(p->name), "%.*s", SRV_PHOTO_NAME_MAX - 1, e->name);
    if (srv_fs_join(srv_photo_root(), e->name, p->path, sizeof(p->path)) != ESP_OK) {
        ESP_LOGW(TAG, "路径过长，跳过 %s", e->name);
        return true;
    }
    p->size = e->size;

    s_count++;
    return true;
}

/* 真正干活的扫描：调用方负责设/清 s_busy */
static esp_err_t scan_impl(void)
{
    if (!srv_sd_is_mounted()) {
        s_count = 0;
        return ESP_ERR_INVALID_STATE;
    }

    if (items_alloc() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    (void)srv_fs_mkdir(srv_photo_root());       /* 不存在就建一个，方便用户拷文件 */

    s_count = 0;
    s_overflow = false;
    s_bad = 0;
    esp_err_t ret = srv_fs_list(srv_photo_root(), add_from_entry, NULL);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "扫描失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 文件系统给的是字典序（1, 10, 100, 2…），这里按自然序重排，
     * 让 1、2、3、10 这样顺下来。排序必须在截断之前，否则收进来的
     * 就是字典序前一批，看着仍是一堆断号。 */
    if (s_count > 1) {
        qsort(s_items, (size_t)s_count, sizeof(srv_photo_t), photo_cmp);
    }

    /* 拍完序只留自然序最小的那一批显示 */
    if (s_count > SRV_PHOTO_MAX) {
        s_count = SRV_PHOTO_MAX;
    }

    ESP_LOGI(TAG, "照片扫描完成：%s 下 %d 张%s", srv_photo_root(), s_count,
             s_bad > 0 ? "（另有尺寸异常的 .raw 已跳过）" : "");
    if (s_bad > 0) {
        ESP_LOGW(TAG, "%d 个 .raw 大小不是 %d 字节（没拷完或不是 photo2raw 转的）",
                 s_bad, (int)SRV_PHOTO_BYTES);
    }
    return ESP_OK;
}

bool srv_photo_busy(void)
{
    return s_busy;
}

esp_err_t srv_photo_scan(void)
{
    s_busy = true;
    esp_err_t ret = scan_impl();
    s_busy = false;
    return ret;
}

/* 后台扫描任务：扫完先清标志，再回调通知（回调里通常把活转给 LVGL 线程） */
static void scan_task(void *arg)
{
    void (*done_cb)(void) = (void (*)(void))arg;

    (void)scan_impl();
    s_busy = false;

    if (done_cb != NULL) {
        done_cb();
    }
    vTaskDelete(NULL);
}

esp_err_t srv_photo_scan_async(void (*done_cb)(void))
{
    if (s_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    s_busy = true;                  /* 先占坑，任务起来之前别再进来一次 */

    /* 栈给足：里面要跑 readdir 与 qsort */
    if (xTaskCreate(scan_task, "photo_scan", 4096, (void *)done_cb, 4, NULL) != pdPASS) {
        s_busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t srv_photo_init(void)
{
    (void)srv_photo_root();
    return srv_photo_scan();
}

/* ===================== 查询 ===================== */

int srv_photo_count(void)
{
    return s_count;
}

esp_err_t srv_photo_get(int idx, const srv_photo_t **out)
{
    if (out == NULL || idx < 0 || idx >= s_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = &s_items[idx];
    return ESP_OK;
}

esp_err_t srv_photo_load(int idx, uint8_t **out_data, size_t *out_bytes)
{
    if (out_data == NULL || out_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const srv_photo_t *p = NULL;
    if (srv_photo_get(idx, &p) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 转换脚本的输出固定 153600 字节；大小不符说明文件损坏或不是本工具生成的 */
    if (p->size != SRV_PHOTO_BYTES) {
        ESP_LOGW(TAG, "%s 大小 %u，应为 %u", p->name,
                 (unsigned)p->size, (unsigned)SRV_PHOTO_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *fp = fopen(p->path, "rb");
    if (fp == NULL) {
        ESP_LOGW(TAG, "打开失败: %s", p->path);
        return ESP_ERR_NOT_FOUND;
    }

    /* 一屏像素约 150KB，放 PSRAM 免得挤内部 RAM */
    uint8_t *buf = heap_caps_malloc(SRV_PHOTO_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = malloc(SRV_PHOTO_BYTES);
    }
    if (buf == NULL) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    size_t rd = fread(buf, 1, SRV_PHOTO_BYTES, fp);
    fclose(fp);
    if (rd != SRV_PHOTO_BYTES) {
        ESP_LOGW(TAG, "读取不完整（%u/%u）: %s", (unsigned)rd,
                 (unsigned)SRV_PHOTO_BYTES, p->path);
        free(buf);
        return ESP_FAIL;
    }

    *out_data  = buf;
    *out_bytes = SRV_PHOTO_BYTES;
    return ESP_OK;
}
