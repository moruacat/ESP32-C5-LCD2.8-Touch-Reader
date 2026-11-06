#include "srv_reader.h"
#include "srv_fs.h"
#include "srv_progress.h"
#include "gbk_table.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static const char *TAG = "srv_reader";

#define TITLE_MAX     72
#define AUTHOR_MAX    48

/*! 读正文时每块多大：太小拖慢 SD 读，太大进度条跳得厉害 */
#define READ_CHUNK    (32 * 1024)

/* 正文（UTF-8，整本放在 PSRAM） */
static char  *s_text     = NULL;
static int    s_text_len = 0;       /* 字节数 */
static int    s_char_cnt = 0;       /* 字符数（码点） */
static int    s_char_pos = 0;       /* 阅读位置，字符偏移 */

/* 「字符偏移 → 字节偏移」的扫描游标，和阅读位置是两回事：
 * 顺序读时从上次落点往后走，跳转/恢复进度时从头扫。 */
static int    s_cur_char = 0;
static int    s_cur_byte = 0;

static char   s_path[SRV_FS_PATH_MAX];
static char   s_title[TITLE_MAX];
static char   s_author[AUTHOR_MAX];
static reader_format_t s_fmt = READER_FMT_NONE;
static int    s_chapter_cnt = 0;
static int    s_chapter_idx = 0;
static int    s_meta_chapters = 0;  /* data.ebfl 里 <book chapters="N"> 声明的章数，0 = 没写 */

/* 当前这一章的名字：从章节文件首部（<chapter><title>…</title></chapter>）解析，
 * 读正文的时候顺手就有，不用再开别的文件。 */
static char   s_chap_title[TITLE_MAX];
static bool   s_chap_has_head = false;

/* 章名索引 chapters.ebfl：一行一名，行序 = 章号。整份读进 PSRAM + 一张行偏移表，
 * 这样"第 N 章叫什么"是 O(1) 且不碰 SD。按需加载（做大纲/显示章名时才读），
 * 打开书本身依旧 0 次额外 IO。 */
static char    *s_toc_buf = NULL;
static int32_t *s_toc_off = NULL;
static int      s_toc_cnt = 0;
static bool     s_toc_tried = false;    /* 找过一次就不再反复去 SD 上碰运气 */

/* 后台打开的状态（见 srv_reader_open_async） */
static volatile bool      s_busy = false;
static volatile esp_err_t s_open_result = ESP_ERR_INVALID_STATE;
static volatile int       s_open_progress = 0;  /*!< 0-100，打开做到哪一步了 */
static char               s_open_path[SRV_FS_PATH_MAX];

/* load_text_file 把「读了多少字节」映射到哪个进度区间。打开流程要读两次
 * 文件（先第一章，再按上次的进度切到那一章），两段各占一块，进度条才
 * 一直是涨的 —— 共用一个区间的话第二段会从区间起点重来，看着像倒退。 */
static int s_prog_lo = 20;
static int s_prog_hi = 70;

/* 当前这次读文件的字节数，给界面显示「已读 / 总共」用 */
static volatile int s_io_done = 0;
static volatile int s_io_total = 0;

/* ===================== 小工具 ===================== */

/* 正文可能有几 MB，优先放 PSRAM，实在不行再退回内部 RAM */
static void *psram_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return (p != NULL) ? p : malloc(n);
}

static int utf8_seq_len(uint8_t c)
{
    if (c < 0x80) {
        return 1;
    }
    if ((c & 0xE0) == 0xC0) {
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;       /* 非法首字节当单字节跳过，别让解析卡住 */
}

/* 整段能不能按 UTF-8 走通。纯 ASCII 也算合法 UTF-8，所以英文 txt 会判成 UTF-8 */
static bool utf8_valid(const uint8_t *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        uint8_t c = s[i];
        if (c < 0x80) {
            i++;
            continue;
        }

        int n;
        if ((c & 0xE0) == 0xC0) {
            n = 2;
        } else if ((c & 0xF0) == 0xE0) {
            n = 3;
        } else if ((c & 0xF8) == 0xF0) {
            n = 4;
        } else {
            return false;
        }

        if (i + (size_t)n > len) {
            return false;
        }
        for (int k = 1; k < n; k++) {
            if ((s[i + k] & 0xC0) != 0x80) {
                return false;
            }
        }
        i += (size_t)n;
    }
    return true;
}

static int utf8_count(const char *s, int len)
{
    int n = 0;
    int i = 0;
    while (i < len) {
        i += utf8_seq_len((uint8_t)s[i]);
        n++;
    }
    return n;
}

static int utf8_put(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int gbk_to_uni(uint16_t code)
{
    int lo = 0;
    int hi = g_gbk_table_size - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint16_t k = g_gbk_table[mid].gbk;
        if (k == code) {
            return (int)g_gbk_table[mid].uni;
        }
        if (k < code) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return -1;
}

static char *gbk_to_utf8(const uint8_t *src, size_t len, size_t *out_len)
{
    /* GBK 双字节变 UTF-8 三字节，最坏 1.5 倍，给 2 倍富余 */
    size_t cap = len * 2 + 4;
    char *dst = psram_alloc(cap);
    if (dst == NULL) {
        return NULL;
    }

    size_t i = 0;
    size_t o = 0;
    while (i < len) {
        uint8_t c = src[i];
        if (c < 0x80) {
            dst[o++] = (char)c;
            i++;
            continue;
        }
        if (i + 1 >= len) {
            break;                  /* 落单的半个字，丢掉 */
        }

        uint16_t code = (uint16_t)((c << 8) | src[i + 1]);
        int uni = gbk_to_uni(code);
        /* 表里没有的（生僻字、繁体扩展）用 U+FFFD 占位，至少不是乱码 */
        o += (size_t)utf8_put(uni < 0 ? 0xFFFD : (uint32_t)uni, dst + o);
        i += 2;
    }

    dst[o] = '\0';
    *out_len = o;
    return dst;
}

/**
 * @brief 把「按字节截断」的长度收回到 UTF-8 字符边界
 *
 * 按字节砍会把最后一个汉字砍成半个，标题栏上就是乱码。先退到最后一个字符的
 * 首字节，再看它是否完整落在 len 之内，不完整就整个丢掉。
 */
static size_t utf8_trim_partial(const char *s, size_t len)
{
    size_t cut = len;
    while (cut > 0 && ((uint8_t)s[cut - 1] & 0xC0) == 0x80) {
        cut--;                          /* 落在续字节上 → 退到首字节之后 */
    }
    if (cut == 0) {
        return len;                     /* 整段都是续字节，异常数据，原样返回 */
    }

    size_t start = cut - 1;
    int    need  = utf8_seq_len((uint8_t)s[start]);
    return (need > 1 && start + (size_t)need > len) ? start : len;
}

/* 从 XML 里抠一个标签的文本，抠不到就输出空串 */
static void xml_text(const char *xml, const char *tag, char *out, size_t out_len)
{
    char open[24];
    char close[24];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);

    out[0] = '\0';

    const char *p = strstr(xml, open);
    if (p == NULL) {
        return;
    }
    p += strlen(open);

    const char *e = strstr(p, close);
    if (e == NULL || e <= p) {
        return;
    }

    size_t n = (size_t)(e - p);
    if (n >= out_len) {
        n = utf8_trim_partial(p, out_len - 1);   /* 别切出半个汉字 */
    }
    memcpy(out, p, n);
    out[n] = '\0';
}

/* 取 <book chapters="N"> 里的 N；没写这个属性返回 0 */
static int xml_book_chapters(const char *xml)
{
    const char *p = strstr(xml, "<book");
    if (p == NULL) {
        return 0;
    }
    const char *tag_end = strchr(p, '>');
    if (tag_end == NULL) {
        return 0;
    }

    static const char KEY[] = "chapters=\"";
    const char *a = strstr(p, KEY);
    if (a == NULL || a > tag_end) {
        return 0;
    }
    return (int)strtol(a + sizeof(KEY) - 1, NULL, 10);
}

/* ===================== 正文加载 ===================== */

/*! 章名索引（chapters.ebfl）最大允许多大：比这还大就当文件坏了 */
#define TOC_MAX_BYTES (2 * 1024 * 1024)

/**
 * @brief 剥掉章节文件的 XML 首部，把正文挪到缓冲开头
 *
 * 首部形如（转换工具写的）：
 *     <chapter>
 *     <title>陨落的天才</title>
 *     <volume>第一卷 少年</volume>
 *     </chapter>
 *     ← 空行之后的都是正文，正文不做 XML 转义
 *
 * 章名顺手存进 s_chap_title —— 读正文时本来就把整个文件读进来了，白拿。
 * 没有首部的（旧书、别的工具生成的）原样不动，章名退回"第 N 章"。
 */
static void ebf_strip_head(char *text, size_t *len)
{
    static const char OPEN[]  = "<chapter>";
    static const char CLOSE[] = "</chapter>";

    if (strncmp(text, OPEN, sizeof(OPEN) - 1) != 0) {
        return;                         /* 没有首部 */
    }

    char *end = strstr(text, CLOSE);
    if (end == NULL) {
        return;                         /* 首部不完整，当纯正文处理 */
    }
    char *body = end + sizeof(CLOSE) - 1;

    /* 只在首部范围内抠 title：临时把正文起点截断，免得正文里恰好也有 <title> */
    char saved = *body;
    *body = '\0';
    xml_text(text, "title", s_chap_title, sizeof(s_chap_title));
    *body = saved;

    while (*body == '\n' || *body == '\r') {
        body++;                         /* 跳过首部与正文之间的空行 */
    }

    size_t off  = (size_t)(body - text);
    size_t rest = (*len > off) ? (*len - off) : 0;
    memmove(text, body, rest);
    text[rest] = '\0';
    *len = rest;

    s_chap_has_head = (s_chap_title[0] != '\0');
}

/**
 * @brief 按需把章名索引读进 PSRAM（做大纲 / 显示章名时才调）
 *
 * 文件格式就是"一行一个章名，行序 = 章号"，所以只要一张行偏移表就能
 * O(1) 查名。整个文件一次读进来、在缓冲里就地断行，之后查名完全不碰 SD。
 * 找不到文件（旧书、别的工具生成的）就记下 s_toc_tried，以后都退回"第 N 章"。
 */
static void toc_load(void)
{
    if (s_toc_tried || s_fmt != READER_FMT_EBF) {
        return;
    }
    s_toc_tried = true;

    char path[SRV_FS_PATH_MAX];
    if (srv_fs_join(s_path, "chapters.ebfl", path, sizeof(path)) != ESP_OK) {
        return;
    }

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return;                         /* 没有索引不算错，回退到"第 N 章" */
    }

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > TOC_MAX_BYTES) {
        fclose(fp);
        ESP_LOGW(TAG, "章名索引大小异常（%ld 字节），改用第 N 章", sz);
        return;
    }

    char *buf = psram_alloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(fp);
        return;
    }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[got] = '\0';

    int lines = (got > 0) ? 1 : 0;
    for (size_t i = 0; i < got; i++) {
        if (buf[i] == '\n') {
            lines++;
        }
    }
    if (lines <= 0) {
        free(buf);
        return;
    }

    int32_t *off = psram_alloc(sizeof(int32_t) * (size_t)(lines + 1));
    if (off == NULL) {
        free(buf);
        return;
    }

    int n = 0;
    off[n++] = 0;
    for (size_t i = 0; i < got; i++) {
        if (buf[i] == '\n') {
            buf[i] = '\0';              /* 就地断行，省一份内存 */
            off[n++] = (int32_t)(i + 1);
        }
    }
    if (n > 0 && off[n - 1] >= (int32_t)got) {
        n--;                            /* 文件以换行收尾，末行是空串，去掉 */
    }

    s_toc_buf = buf;
    s_toc_off = off;
    s_toc_cnt = n;

    ESP_LOGI(TAG, "章名索引载入 %d 条（%ld 字节）", n, sz);
}

/**
 * @brief 读一个文本文件，转成 UTF-8 塞进 s_text
 *
 * 会先释放旧正文，所以切章节可以直接复用它。
 */
static esp_err_t load_text_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        ESP_LOGW(TAG, "打不开 %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > SRV_READER_TEXT_MAX) {
        fclose(fp);
        ESP_LOGW(TAG, "文件大小 %ld 超出范围（上限 %d）", sz, SRV_READER_TEXT_MAX);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *raw = psram_alloc((size_t)sz + 4);
    if (raw == NULL) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    /* 分块读而不是一次 fread：SD 上整章几 MB 要读好几百毫秒，分块后
     * 每块都能把进度报给界面（s_open_progress），进度条才会真的动 */
    s_io_total = (int)sz;
    s_io_done = 0;

    size_t got = 0;
    while (got < (size_t)sz) {
        size_t want = (size_t)sz - got;
        if (want > READ_CHUNK) {
            want = READ_CHUNK;
        }
        size_t n = fread(raw + got, 1, want, fp);
        if (n == 0) {
            break;
        }
        got += n;
        s_io_done = (int)got;
        s_open_progress = s_prog_lo +
                          (int)((int64_t)got * (s_prog_hi - s_prog_lo) / sz);
    }
    fclose(fp);

    size_t off = 0;
    if (got >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        off = 3;                    /* 跳过 UTF-8 BOM，否则开头会多一个不可见字符 */
    }
    got -= off;

    char  *text = NULL;
    size_t tlen = 0;

    if (utf8_valid(raw + off, got)) {
        text = psram_alloc(got + 1);
        if (text != NULL) {
            memcpy(text, raw + off, got);
            text[got] = '\0';
            tlen = got;
        }
    } else {
        text = gbk_to_utf8(raw + off, got, &tlen);
        if (text != NULL) {
            ESP_LOGI(TAG, "按 GBK 解码：%u 字节 → %u 字节 UTF-8",
                     (unsigned)got, (unsigned)tlen);
        }
    }
    free(raw);

    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 章节文件带 XML 首部（章名/卷名），剥掉之后 s_text 里就只剩正文 */
    s_chap_title[0] = '\0';
    s_chap_has_head = false;
    ebf_strip_head(text, &tlen);

    free(s_text);
    s_text     = text;
    s_text_len = (int)tlen;
    s_char_cnt = utf8_count(s_text, s_text_len);
    s_char_pos = 0;
    s_cur_char = 0;
    s_cur_byte = 0;

    ESP_LOGI(TAG, "载入 %s：%d 字节 / %d 字%s%s",
             srv_fs_basename(path), s_text_len, s_char_cnt,
             s_chap_has_head ? "，章名 " : "", s_chap_title);
    return ESP_OK;
}

/* 第 n 章的文件在不在（n 从 1 开始） */
static bool ebf_chapter_exists(const char *dir, int n)
{
    char name[24];
    char path[SRV_FS_PATH_MAX];

    if (snprintf(name, sizeof(name), "chapter.%d.ebfl", n) <= 0) {
        return false;
    }
    return srv_fs_join(dir, name, path, sizeof(path)) == ESP_OK && srv_fs_exists(path);
}

/**
 * @brief 数 EBF 有多少章（**兜底手段**，正常都直接读 data.ebfl）
 *
 * 章节数由转换工具写进 `<book chapters="N">`，固件直接读那个数就完事，
 * 一次 stat 都不用。只有遇到没有这个属性的书（别的工具生成的、旧版本转的、
 * 手写的）才走这里现场数。
 *
 * 数法是「倍增 + 二分」：EBF 约定章节号从 1 连续，所以「第 n 章存在」对 n
 * 单调，先按 2/4/8… 倍增探到第一个不存在的编号作为上界，再在上界与上一个
 * 已知存在的编号之间二分。几万章也就三十来次 stat，逐章探测则要几万次
 * SD 目录查找 —— 打开慢就慢在那里。这里不设章数上限，只受 int 溢出限制。
 */
static int ebf_count_chapters(const char *dir)
{
    if (!ebf_chapter_exists(dir, 1)) {
        return 0;                       /* 连第一章都没有，当空书 */
    }

    int lo = 1;                         /* 已知存在 */
    int hi = 0;                         /* 0 = 还没探到不存在的编号 */

    for (int probe = 2; hi == 0; probe *= 2) {
        if (ebf_chapter_exists(dir, probe)) {
            lo = probe;
            if (probe > INT_MAX / 2) {
                break;                  /* 再翻倍就溢出了，到此为止（不会真到这一步） */
            }
        } else {
            hi = probe;
        }
    }
    if (hi == 0) {
        return lo;                      /* 一路探到 int 顶都没探空，就按已知的算 */
    }

    while (hi - lo > 1) {
        int mid = lo + (hi - lo) / 2;
        if (ebf_chapter_exists(dir, mid)) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static void ebf_load_meta(const char *dir)
{
    char path[SRV_FS_PATH_MAX];
    if (srv_fs_join(dir, "data.ebfl", path, sizeof(path)) != ESP_OK) {
        return;
    }

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return;
    }

    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';

    xml_text(buf, "title", s_title, sizeof(s_title));
    xml_text(buf, "author", s_author, sizeof(s_author));
    s_meta_chapters = xml_book_chapters(buf);
}

/* ===================== 生命周期 ===================== */

void srv_reader_close(void)
{
    free(s_text);
    s_text = NULL;
    s_text_len = 0;
    s_char_cnt = 0;
    s_char_pos = 0;
    s_cur_char = 0;
    s_cur_byte = 0;

    free(s_toc_buf);
    free(s_toc_off);
    s_toc_buf = NULL;
    s_toc_off = NULL;
    s_toc_cnt = 0;
    s_toc_tried = false;

    s_chap_title[0] = '\0';
    s_chap_has_head = false;

    s_fmt = READER_FMT_NONE;
    s_chapter_cnt = 0;
    s_chapter_idx = 0;
    s_meta_chapters = 0;
    s_title[0] = '\0';
    s_author[0] = '\0';
    s_path[0] = '\0';
}

esp_err_t srv_reader_open(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    srv_reader_close();
    s_open_progress = 5;
    s_io_done = 0;                      /* 还没开始读正文 */
    s_io_total = 0;
    s_prog_lo = 20;                     /* 本段：（读第一章正文）映射到 20~70 */
    s_prog_hi = 70;

    if (srv_fs_is_dir(path)) {
        /* ---- EBF：一本书是一个目录 ---- */
        /* 先读元数据：章数就写在 <book chapters="N"> 里，读到了就不用去数文件 */
        ebf_load_meta(path);
        s_open_progress = 12;

        int cnt = (s_meta_chapters > 0) ? s_meta_chapters : ebf_count_chapters(path);
        if (cnt <= 0) {
            ESP_LOGW(TAG, "%s 下没有 chapter.N.ebfl", path);
            return ESP_ERR_NOT_FOUND;
        }
        s_open_progress = 20;           /* 下面读第一章，20→70 由读入进度决定 */

        snprintf(s_path, sizeof(s_path), "%.*s", (int)(sizeof(s_path) - 1), path);
        s_fmt = READER_FMT_EBF;
        s_chapter_cnt = cnt;
        s_chapter_idx = 0;

        if (s_title[0] == '\0') {
            snprintf(s_title, sizeof(s_title), "%.*s",
                     (int)(sizeof(s_title) - 1), srv_fs_basename(path));
        }

        esp_err_t ret = srv_reader_goto_chapter(0);
        if (ret != ESP_OK) {
            srv_reader_close();
            return ret;
        }

        ESP_LOGI(TAG, "打开 EBF《%s》共 %d 章", s_title, cnt);
        return ESP_OK;
    }

    /* ---- TXT：一本书就是一个文件 ---- */
    snprintf(s_path, sizeof(s_path), "%.*s", (int)(sizeof(s_path) - 1), path);

    s_open_progress = 20;               /* 整本一次读入，20→70 由读入进度决定 */
    esp_err_t ret = load_text_file(path);
    if (ret != ESP_OK) {
        s_fmt = READER_FMT_NONE;
        return ret;
    }

    s_fmt = READER_FMT_TXT;
    s_chapter_cnt = 1;
    s_chapter_idx = 0;
    s_author[0] = '\0';

    /* 书名取文件名去掉扩展名 */
    snprintf(s_title, sizeof(s_title), "%.*s",
             (int)(sizeof(s_title) - 1), srv_fs_basename(path));
    char *dot = strrchr(s_title, '.');
    if (dot != NULL && dot != s_title) {
        *dot = '\0';
    }

    ESP_LOGI(TAG, "打开 TXT《%s》%d 字", s_title, s_char_cnt);
    return ESP_OK;
}

bool srv_reader_is_open(void)
{
    return s_fmt != READER_FMT_NONE;
}

reader_format_t srv_reader_format(void)
{
    return s_fmt;
}

const char *srv_reader_title(void)
{
    return s_title;
}

const char *srv_reader_author(void)
{
    return s_author;
}

const char *srv_reader_path(void)
{
    return s_path;
}

int srv_reader_char_count(void)
{
    return s_char_cnt;
}

int srv_reader_chapter_count(void)
{
    return s_chapter_cnt;
}

int srv_reader_chapter_index(void)
{
    return s_chapter_idx;
}

/* 拷字符串，放不下就截断 —— 并且退到 UTF-8 字符边界，别切出半个汉字 */
static void copy_str(char *dst, size_t dst_len, const char *src)
{
    int n = snprintf(dst, dst_len, "%s", src);
    if (n > 0 && (size_t)n >= dst_len) {
        dst[utf8_trim_partial(dst, dst_len - 1)] = '\0';
    }
}

void srv_reader_chapter_title(int idx, char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0) {
        return;
    }
    if (idx < 0 || idx >= s_chapter_cnt) {
        buf[0] = '\0';
        return;
    }

    /* 当前这一章：首部里就有（读正文时顺手解析的），一次 IO 都不用 */
    if (idx == s_chapter_idx && s_chap_has_head) {
        copy_str(buf, buf_len, s_chap_title);
        return;
    }

    /* 其它章：查章名索引（首次调用时才去读，之后纯内存） */
    toc_load();
    if (s_toc_off != NULL && idx < s_toc_cnt) {
        copy_str(buf, buf_len, s_toc_buf + s_toc_off[idx]);
        return;
    }

    snprintf(buf, buf_len, "第 %d 章", idx + 1);     /* 没有索引就退回章号 */
}

esp_err_t srv_reader_goto_chapter(int idx)
{
    if (s_fmt != READER_FMT_EBF) {
        return ESP_ERR_INVALID_STATE;
    }
    if (idx < 0 || idx >= s_chapter_cnt) {
        return ESP_ERR_INVALID_ARG;
    }

    char name[24];
    char path[SRV_FS_PATH_MAX];
    snprintf(name, sizeof(name), "chapter.%d.ebfl", idx + 1);
    if (srv_fs_join(s_path, name, path, sizeof(path)) != ESP_OK) {
        return ESP_FAIL;
    }

    esp_err_t ret = load_text_file(path);   /* 内部会释放上一章 */
    if (ret == ESP_OK) {
        s_chapter_idx = idx;
    }
    return ret;
}

/* ===================== 取文本 ===================== */

/**
 * @brief 把「字符偏移 → 字节偏移」的游标推到 char_off，返回字节偏移
 *
 * 顺序读时接着上次的落点往后走，跳转或恢复进度时从头扫一遍。
 * 整本 4MB 也就两百万字，扫一遍是毫秒级的，不值得再维护索引表。
 */
static int cursor_to(int char_off)
{
    int byte;
    int ch;

    if (char_off >= s_cur_char) {
        byte = s_cur_byte;
        ch   = s_cur_char;
    } else {
        byte = 0;
        ch   = 0;
    }

    while (ch < char_off && byte < s_text_len) {
        byte += utf8_seq_len((uint8_t)s_text[byte]);
        ch++;
    }

    s_cur_char = ch;
    s_cur_byte = byte;
    return byte;
}

esp_err_t srv_reader_read(int char_off, int char_cnt, char *out, size_t out_len, int *read_cnt)
{
    if (s_text == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out == NULL || out_len == 0 || char_cnt <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (read_cnt != NULL) {
        *read_cnt = 0;
    }
    if (char_off < 0) {
        char_off = 0;
    }

    int byte = cursor_to(char_off);
    int n = 0;
    size_t o = 0;

    while (byte < s_text_len && n < char_cnt) {
        int l = utf8_seq_len((uint8_t)s_text[byte]);
        if (o + (size_t)l + 1 > out_len) {
            break;                  /* 缓冲放不下下一个字了 */
        }
        memcpy(out + o, s_text + byte, l);
        o += (size_t)l;
        byte += l;
        n++;
    }

    out[o] = '\0';
    if (read_cnt != NULL) {
        *read_cnt = n;
    }
    return ESP_OK;
}

int srv_reader_position(void)
{
    return s_char_pos;
}

esp_err_t srv_reader_seek(int char_off)
{
    if (s_text == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (char_off < 0) {
        char_off = 0;
    }
    if (char_off > s_char_cnt) {
        char_off = s_char_cnt;
    }
    s_char_pos = char_off;
    return ESP_OK;
}

/* ===================== 进度 ===================== */

/*! force = false 时由 srv_progress 按最小间隔节流（翻页频繁，别每页都写 SD） */
static esp_err_t reader_save_progress(bool force)
{
    if (s_fmt == READER_FMT_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    srv_progress_reader_t p = {0};
    p.chapter = s_chapter_idx;
    /* 这个字段原本叫 byte_off，这里存的是**字符偏移**：按字节存的话，
     * 换个编码（UTF-8 / GBK）字数就变了，对不上；字符数是稳定的。 */
    p.byte_off = s_char_pos;
    p.pct = (s_char_cnt > 0) ? (s_char_pos * 100 / s_char_cnt) : 0;

    return srv_progress_reader_save(s_path, &p, force);
}

esp_err_t srv_reader_save_progress(void)
{
    return reader_save_progress(false);
}

esp_err_t srv_reader_save_progress_force(void)
{
    return reader_save_progress(true);
}

esp_err_t srv_reader_load_progress(void)
{
    if (s_fmt == READER_FMT_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    srv_progress_reader_t p;
    if (srv_progress_reader_load(s_path, &p) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    if (s_fmt == READER_FMT_EBF && p.chapter != s_chapter_idx &&
        p.chapter >= 0 && p.chapter < s_chapter_cnt) {
        if (srv_reader_goto_chapter(p.chapter) != ESP_OK) {
            return ESP_FAIL;
        }
    }

    return srv_reader_seek(p.byte_off);
}

/* ===================== 后台打开 ===================== */

static void reader_open_task(void *arg)
{
    (void)arg;

    esp_err_t ret = srv_reader_open(s_open_path);
    if (ret == ESP_OK) {
        /* 读进度也在这儿做完：进度指向别的章时它会顺手把那一章读进来，
         * 留在 UI 线程做就是又一次整章的 SD 读，照样卡。
         * 这一段可能还要读一次正文，所以给它 70~95 的区间。 */
        s_prog_lo = 70;
        s_prog_hi = 95;
        srv_reader_load_progress();
    }

    s_open_progress = 100;
    s_open_result = ret;
    s_busy = false;                     /* 必须最后写：UI 靠它判断结果可用 */
    vTaskDelete(NULL);
}

int srv_reader_open_progress(void)
{
    return s_open_progress;
}

void srv_reader_open_bytes(int *done, int *total)
{
    if (done != NULL) {
        *done = s_io_done;
    }
    if (total != NULL) {
        *total = s_io_total;
    }
}

esp_err_t srv_reader_open_async(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_busy) {
        return ESP_ERR_INVALID_STATE;   /* 上一本还没开完 */
    }

    int n = snprintf(s_open_path, sizeof(s_open_path), "%s", path);
    if (n <= 0 || (size_t)n >= sizeof(s_open_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    s_open_result = ESP_ERR_INVALID_STATE;
    s_open_progress = 0;
    s_busy = true;

    /* 打开过程里有 2KB 的 XML 缓冲、还要整章读文本，栈给足；
     * 优先级压到 UI 之下，SD 读写别去跟 LVGL 抢 CPU */
    if (xTaskCreate(reader_open_task, "rd_open", 6144, NULL, 4, NULL) != pdPASS) {
        s_busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool srv_reader_busy(void)
{
    return s_busy;
}

esp_err_t srv_reader_open_result(void)
{
    return s_open_result;
}
