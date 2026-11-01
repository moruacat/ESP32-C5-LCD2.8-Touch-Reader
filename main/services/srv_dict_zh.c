#include "srv_dict_zh.h"
#include "srv_sd.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "srv_dict_zh";

#define BLOCK        64
#define HEAD_BYTES   8
#define LINE_MAX     512
#define ZH_DIR       "/sdcard/dict"

typedef struct {
    uint32_t offset;                 /*!< 主数据文件偏移 */
    uint8_t  key[HEAD_BYTES];       /*!< 块首词（UTF-8 前 8 字节） */
} anchor_t;

typedef struct {
    uint8_t  key[HEAD_BYTES];       /*!< 块首拼音（无声调前 8 字节） */
    uint32_t start_idx;             /*!< 该块在行号表里的起始序号 */
} py_anchor_t;

typedef struct {
    uint32_t count;                  /*!< 主数据总行数 */
    uint32_t anchors;                /*!< 锚点数 */
    anchor_t *tab;                   /*!< 主锚点表 */
} zh_index_t;

typedef struct {
    uint32_t count;                  /*!< 拼音索引总条数 */
    uint32_t anchors;                /*!< 锚点数 */
    py_anchor_t *tab;
    FILE    *fp;                     /*!< 行号表所在文件（常开） */
    long     rows_ofs;               /*!< 行号表起始偏移 */
} zh_py_index_t;

static FILE       *s_dat;            /*!< 正文常开 */
static zh_index_t  s_idx;
static zh_py_index_t s_py;
static bool        s_ready;

/* ===================== 基础 ===================== */

/* 无声调拼音：去掉数字与空格，统一小写（数据端已经是无声调，这里只做归一） */
static void py_norm(const char *src, char *dst, size_t cap)
{
    size_t n = 0;
    for (const char *p = src; *p != '\0' && n + 1 < cap; p++) {
        char c = *p;
        if (c == ' ' || (c >= '0' && c <= '9')) {
            continue;
        }
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        dst[n++] = c;
    }
    dst[n] = '\0';
}

/* 解析一行 5 字段，填 srv_dict_entry_t */
static bool parse_line(char *line, srv_dict_entry_t *e)
{
    memset(e, 0, sizeof(*e));

    char *fields[5] = {0};
    int n = 0;
    char *p = line;
    for (; n < 5 && p != NULL; n++) {
        fields[n] = p;
        char *tab = strchr(p, '\t');
        if (tab == NULL) {
            break;
        }
        *tab = '\0';
        p = tab + 1;
    }
    if (n < 2) {
        return false;
    }

    snprintf(e->word, sizeof(e->word), "%s", fields[0]);
    snprintf(e->phonetic, sizeof(e->phonetic), "%s", fields[1]);
    if (n >= 3) {
        snprintf(e->def, sizeof(e->def), "%s", fields[2]);
    }
    if (n >= 5) {
        snprintf(e->lemma, sizeof(e->lemma), "%s", fields[4]);
    }
    /* 去掉行尾换行 */
    size_t l = strlen(e->def);
    while (l > 0 && (e->def[l - 1] == '\n' || e->def[l - 1] == '\r')) {
        e->def[--l] = '\0';
    }
    return e->word[0] != '\0';
}

/* 从正文偏移处读一行并解析 */
static bool read_at(uint32_t offset, srv_dict_entry_t *e)
{
    if (!s_ready || s_dat == NULL) {
        return false;
    }
    if (fseek(s_dat, (long)offset, SEEK_SET) != 0) {
        return false;
    }
    char line[LINE_MAX];
    if (fgets(line, sizeof(line), s_dat) == NULL) {
        return false;
    }
    return parse_line(line, e);
}

/* 从块偏移开始顺序读一行（调用方自己维护 fseek 位置） */
static bool read_next(srv_dict_entry_t *e)
{
    char line[LINE_MAX];
    if (s_dat == NULL || fgets(line, sizeof(line), s_dat) == NULL) {
        return false;
    }
    return parse_line(line, e);
}

/* 二分锚点：最后一个 key <= target 的块号 */
static uint32_t find_block(const char *target)
{
    uint32_t lo = 0, hi = s_idx.anchors;
    while (lo + 1 < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (memcmp(s_idx.tab[mid].key, target, HEAD_BYTES) <= 0) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* ===================== 初始化 ===================== */

static void *psram_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

static bool load_main_index(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        ESP_LOGW(TAG, "缺少 %s", path);
        return false;
    }

    uint32_t head[2] = {0};
    if (fread(head, sizeof(uint32_t), 2, fp) != 2 || head[1] == 0) {
        fclose(fp);
        return false;
    }

    s_idx.count   = head[0];
    s_idx.anchors = head[1];
    s_idx.tab = psram_malloc(sizeof(anchor_t) * s_idx.anchors);
    if (s_idx.tab == NULL) {
        fclose(fp);
        return false;
    }

    for (uint32_t i = 0; i < s_idx.anchors; i++) {
        uint32_t no;
        uint8_t key[HEAD_BYTES];
        if (fread(&no, sizeof(uint32_t), 1, fp) != 1 ||
            fread(key, 1, HEAD_BYTES, fp) != HEAD_BYTES) {
            free(s_idx.tab);
            s_idx.tab = NULL;
            fclose(fp);
            return false;
        }
        s_idx.tab[i].offset = no;
        memcpy(s_idx.tab[i].key, key, HEAD_BYTES);
    }

    fclose(fp);
    ESP_LOGI(TAG, "主索引：%u 行，%u 个锚点", s_idx.count, s_idx.anchors);
    return true;
}

static bool load_py_index(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        ESP_LOGW(TAG, "缺少 %s", path);
        return false;
    }

    uint32_t head[2] = {0};
    if (fread(head, sizeof(uint32_t), 2, fp) != 2 || head[1] == 0) {
        fclose(fp);
        return false;
    }

    s_py.count   = head[0];
    s_py.anchors = head[1];
    s_py.tab = psram_malloc(sizeof(py_anchor_t) * s_py.anchors);
    if (s_py.tab == NULL) {
        fclose(fp);
        return false;
    }

    for (uint32_t i = 0; i < s_py.anchors; i++) {
        uint32_t start, pad;
        uint8_t key[HEAD_BYTES];
        if (fread(key, 1, HEAD_BYTES, fp) != HEAD_BYTES ||
            fread(&start, sizeof(uint32_t), 1, fp) != 1 ||
            fread(&pad, sizeof(uint32_t), 1, fp) != 1) {
            free(s_py.tab);
            s_py.tab = NULL;
            fclose(fp);
            return false;
        }
        memcpy(s_py.tab[i].key, key, HEAD_BYTES);
        s_py.tab[i].start_idx = start;
    }

    s_py.rows_ofs = ftell(fp);
    s_py.fp = fp;                   /* 常开：拼音查找要逐条读行号 */
    ESP_LOGI(TAG, "拼音索引：%u 条，%u 个锚点", s_py.count, s_py.anchors);
    return true;
}

esp_err_t srv_dict_zh_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (!srv_sd_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    char path[128];

    snprintf(path, sizeof(path), "%s/zhdict.idx", ZH_DIR);
    if (!load_main_index(path)) {
        return ESP_ERR_NOT_FOUND;
    }

    snprintf(path, sizeof(path), "%s/zhdict_py.idx", ZH_DIR);
    if (!load_py_index(path)) {
        return ESP_ERR_NOT_FOUND;
    }

    snprintf(path, sizeof(path), "%s/zhdict.dat", ZH_DIR);
    s_dat = fopen(path, "rb");
    if (s_dat == NULL) {
        ESP_LOGW(TAG, "缺少 %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    s_ready = true;
    return ESP_OK;
}

bool srv_dict_zh_ready(void)
{
    return s_ready;
}

/* ===================== 查询 ===================== */

esp_err_t srv_dict_zh_lookup(const char *word, srv_dict_entry_t *out)
{
    if (!s_ready || word == NULL || out == NULL || word[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t block = find_block(word);
    uint32_t start = s_idx.tab[block].offset;
    uint32_t first = block * BLOCK;
    uint32_t last = first + BLOCK;
    if (last > s_idx.count) {
        last = s_idx.count;
    }

    if (fseek(s_dat, (long)start, SEEK_SET) != 0) {
        return ESP_FAIL;
    }

    /* 块内有序，最多 64 行 */
    for (uint32_t no = first; no < last; no++) {
        srv_dict_entry_t e;
        if (!read_next(&e)) {
            return ESP_FAIL;
        }
        int cmp = strcmp(e.word, word);
        if (cmp == 0) {
            *out = e;
            return ESP_OK;
        }
        if (cmp > 0) {
            break;                  /* 越过了就没有 */
        }
    }
    return ESP_ERR_NOT_FOUND;
}

int srv_dict_zh_prefix(const char *prefix, srv_dict_list_cb_t cb, void *arg, int max)
{
    if (!s_ready || prefix == NULL || cb == NULL || prefix[0] == '\0' || max <= 0) {
        return 0;
    }

    uint32_t block = find_block(prefix);
    const size_t plen = strlen(prefix);
    int n = 0;

    /* 从块首开始顺序读，直到越过前缀段。锚点块可能从更早的词开始，
     * 所以先跳过小于前缀的词；命中可能跨块，这里一直读到不匹配为止。 */
    if (fseek(s_dat, (long)s_idx.tab[block].offset, SEEK_SET) != 0) {
        return 0;
    }
    for (uint32_t no = block * BLOCK; no < s_idx.count && n < max; no++) {
        srv_dict_entry_t e;
        if (!read_next(&e)) {
            break;
        }
        if (strncmp(e.word, prefix, plen) == 0) {
            if (!cb(&e, arg)) {
                break;
            }
            n++;
            continue;
        }
        /* 不匹配：已经排到前缀后面就结束，否则继续往后找 */
        if (strcmp(e.word, prefix) > 0) {
            break;
        }
    }
    return n;
}

int srv_dict_zh_pinyin(const char *pinyin, srv_dict_list_cb_t cb, void *arg, int max)
{
    if (!s_ready || pinyin == NULL || cb == NULL || pinyin[0] == '\0' || max <= 0 ||
        s_py.fp == NULL) {
        return 0;
    }

    char target[64];
    py_norm(pinyin, target, sizeof(target));
    if (target[0] == '\0') {
        return 0;
    }

    /* 二分拼音锚点 */
    uint32_t lo = 0, hi = s_py.anchors;
    uint8_t key[HEAD_BYTES];
    memset(key, 0, sizeof(key));
    const size_t tl = strlen(target);
    memcpy(key, target, tl < HEAD_BYTES ? tl : HEAD_BYTES);
    while (lo + 1 < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (memcmp(s_py.tab[mid].key, key, HEAD_BYTES) <= 0) {
            lo = mid;
        } else {
            hi = mid;
        }
    }

    const uint32_t start = s_py.tab[lo].start_idx;
    uint32_t end = start + BLOCK;
    if (end > s_py.count) {
        end = s_py.count;
    }

    int n = 0;
    for (uint32_t i = start; i < end && n < max; i++) {
        uint32_t offset = 0;
        fseek(s_py.fp, s_py.rows_ofs + (long)i * 4, SEEK_SET);
        if (fread(&offset, sizeof(uint32_t), 1, s_py.fp) != 1) {
            break;
        }
        srv_dict_entry_t e;
        if (!read_at(offset, &e)) {
            continue;
        }
        char py[64];
        py_norm(e.phonetic, py, sizeof(py));
        if (strcmp(py, target) != 0) {
            continue;
        }
        if (!cb(&e, arg)) {
            break;
        }
        n++;
    }
    return n;
}
