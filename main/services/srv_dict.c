#include "srv_dict.h"

#include "srv_sd.h"

#include "esp_log.h"
#include "esp_heap_caps.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "srv_dict";

/*! 主表一行的缓冲（生成器保证行长 ≤640） */
#define DICT_LINE_MAX   640
/*! 锚点里记的块首词前缀字节数（与 tools/build_dict.py 的 KEY_BYTES 一致） */
#define ANCHOR_KEY_BYTES 8
/*! 拼写容错最多扫这么多字节就放弃 */
#define SUGGEST_MAX_BYTES (64 * 1024)
/*! 两字母桶数量：aa..zz */
#define PAIR_COUNT      (26 * 26)
/*! 词形还原最多生成几个候选 */
#define STEM_MAX        24

/*! 索引锚点 */
typedef struct {
    uint32_t off;                       /*!< 该块首行的文件偏移 */
    char     key[ANCHOR_KEY_BYTES + 1]; /*!< 块首词前 8 字节（NUL 结尾，方便日志） */
} dict_anchor_t;

static char   s_path_dat[80];
static char   s_path_idx[80];

static FILE  *s_fp = NULL;
static dict_anchor_t *s_anchors = NULL;     /*!< PSRAM 常驻 */
static int    s_anchor_n = 0;
static int    s_step = 64;
static int    s_count = 0;
static uint32_t s_main_size = 0;
static bool   s_ready = false;

/*! 每个两字母桶的起始锚点下标（PAIR_COUNT+1 项，最后一项 = 锚点总数） */
static uint16_t *s_pair_from = NULL;

/*! 解析一行用的可变缓冲（parse_entry 会就地切字段）。
 *  查词是单线程的（UI 线程），所以全服务共用一个，省任务栈与 .bss。 */
static char s_parse_buf[DICT_LINE_MAX];

/*! 拼写容错：候选上限 + 排序依据。
 *  只存「词 + 徽章」，10 几个词条才 2KB 多；**不能收满就停**，否则像 aple→apple
 *  这种常用词会因为前面先撞上一堆冷门同形词而被截掉。 */
#define SUGGEST_KEEP     64
#define SUGGEST_WORD_MAX 40

typedef struct {
    char     word[SUGGEST_WORD_MAX];
    uint16_t tags;
    uint8_t  collins;
} sug_item_t;

static sug_item_t s_sug_items[SUGGEST_KEEP];

/* 常用度打分：考纲/牛津标签数量 + Collins 星数（apple 的 高考+中考+牛津+3星
 * 明显高于 ape 的 托福+雅思+1星，拼写建议里会排前面） */
static int sug_score(uint16_t tags, uint8_t collins)
{
    int n = 0;
    for (uint16_t t = tags; t != 0; t >>= 1) {
        n += (t & 1u);
    }
    return n + (int)collins;
}

/* 候选比较：常用度高的优先，其次长度更接近，最后字母序 */
static bool sug_better(const sug_item_t *a, const sug_item_t *b, int wlen)
{
    const int sa = sug_score(a->tags, a->collins);
    const int sb = sug_score(b->tags, b->collins);
    if (sa != sb) {
        return sa > sb;
    }
    const int da = abs((int)strlen(a->word) - wlen);
    const int db = abs((int)strlen(b->word) - wlen);
    if (da != db) {
        return da < db;
    }
    return strcmp(a->word, b->word) < 0;
}

static void sug_insert(sug_item_t *items, int *n, const sug_item_t *item, int wlen)
{
    /* 满了以后只有比当前最差的更好才替换，保证常用词不会被一堆冷门词挤出去 */
    if (*n >= SUGGEST_KEEP) {
        if (!sug_better(item, &items[*n - 1], wlen)) {
            return;
        }
        items[*n - 1] = *item;
    } else {
        items[*n] = *item;
        (*n)++;
    }

    /* 插入排序（n <= 64，代价可忽略） */
    for (int i = *n - 1; i > 0; i--) {
        if (!sug_better(&items[i], &items[i - 1], wlen)) {
            break;
        }
        sug_item_t tmp = items[i];
        items[i] = items[i - 1];
        items[i - 1] = tmp;
    }
}

/* ===================== 路径与小工具 ===================== */

const char *srv_dict_path(void)
{
    if (s_path_dat[0] == '\0') {
        snprintf(s_path_dat, sizeof(s_path_dat), "%s/dict/dict.dat", srv_sd_mount_point());
    }
    return s_path_dat;
}

bool srv_dict_ready(void)
{
    return s_ready;
}

/* 去掉行尾 CR/LF */
static char *chomp(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
    return s;
}

/* 定长拷贝（不用 snprintf：源是整行缓冲，编译器会报 format-truncation） */
static void copy_word(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n > cap - 1) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/**
 * @brief 把一行（5 字段）解析成词条
 *
 * 只在这里做「找 TAB」这一遍；查词主循环里用前缀比对先把候选挡掉，命中才调它。
 */
static bool parse_entry(char *line, srv_dict_entry_t *e)
{
    char *f[5] = { NULL, NULL, NULL, NULL, NULL };
    int   n = 0;

    char *p = line;
    while (n < 5) {
        f[n++] = p;
        char *tab = strchr(p, '\t');
        if (tab == NULL) {
            break;
        }
        *tab = '\0';
        p = tab + 1;
    }
    if (n != 5) {
        return false;
    }

    memset(e, 0, sizeof(*e));
    snprintf(e->word, sizeof(e->word), "%s", f[0]);
    snprintf(e->phonetic, sizeof(e->phonetic), "%s", f[1]);
    snprintf(e->def, sizeof(e->def), "%s", f[2]);
    snprintf(e->lemma, sizeof(e->lemma), "%s", f[4]);

    for (const char *q = f[3]; *q != '\0'; q++) {
        switch (*q) {
        case '4': e->tags |= SRV_DICT_TAG_CET4;   break;
        case '6': e->tags |= SRV_DICT_TAG_CET6;   break;
        case 'K': e->tags |= SRV_DICT_TAG_KY;     break;
        case 'G': e->tags |= SRV_DICT_TAG_GK;     break;
        case 'Z': e->tags |= SRV_DICT_TAG_ZK;     break;
        case 'T': e->tags |= SRV_DICT_TAG_TOEFL;  break;
        case 'I': e->tags |= SRV_DICT_TAG_IELTS;  break;
        case 'R': e->tags |= SRV_DICT_TAG_GRE;    break;
        case 'O': e->tags |= SRV_DICT_TAG_OXFORD; break;
        case 'C':
            if (q[1] >= '1' && q[1] <= '5') {
                e->collins = (uint8_t)(q[1] - '0');
                q++;
            }
            break;
        default:
            break;
        }
    }
    return e->word[0] != '\0';
}

/* ===================== 索引 ===================== */

/* 前 8 字节的"目标前缀"：不足补 0，便于和锚点 key 直接 memcmp */
static void key_of(const char *word, char out[ANCHOR_KEY_BYTES])
{
    memset(out, 0, ANCHOR_KEY_BYTES);
    size_t n = strlen(word);
    if (n > ANCHOR_KEY_BYTES) {
        n = ANCHOR_KEY_BYTES;
    }
    memcpy(out, word, n);
}

/**
 * @brief 二分找「最后一个 key <= word 前 8 字节」的锚点（等价 upper_bound-1）
 *
 * 锚点 key 是块首词的前 8 字节（截断），所以偶尔会落在目标词所在块的前一块；
 * 调用方顺序读时**允许多读一块**即可，不影响正确性。
 */
static int anchor_of(const char *word)
{
    char tk[ANCHOR_KEY_BYTES];
    key_of(word, tk);

    int lo = 0, hi = s_anchor_n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (memcmp(s_anchors[mid].key, tk, ANCHOR_KEY_BYTES) <= 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return (lo > 0) ? (lo - 1) : 0;
}

/* 定位到锚点并顺序读一行；没有下一行返回 false */
static bool read_line_at(int ai)
{
    if (fseek(s_fp, (long)s_anchors[ai].off, SEEK_SET) != 0) {
        return false;
    }
    return true;
}

/* ===================== 精确查找 ===================== */

/**
 * @brief 找词（不区分大小写）
 *
 * 从锚点定位的块开始顺序读，最多连读两块（`2 * step` 行）：命中即解析返回，
 * 一旦「行首词 > 目标词」就收工。
 */
static bool find_word(const char *word, srv_dict_entry_t *out)
{
    if (!s_ready || s_fp == NULL || word[0] == '\0') {
        return false;
    }

    const int ai = anchor_of(word);
    if (!read_line_at(ai)) {
        return false;
    }

    const size_t n = strlen(word);
    char  line[DICT_LINE_MAX];

    for (int i = 0; i < 2 * s_step; i++) {
        if (fgets(line, sizeof(line), s_fp) == NULL) {
            return false;
        }
        int c = strncasecmp(line, word, n);
        if (c != 0) {
            if (c > 0) {
                return false;           /* 已越过目标词 */
            }
            continue;                   /* 目标词还在后面 */
        }
        if (line[n] == '\t') {          /* 卡在 TAB 上 = 整个词相等 */
            if (out == NULL) {
                return true;
            }
            memcpy(s_parse_buf, line, sizeof(s_parse_buf));
            return parse_entry(chomp(s_parse_buf), out);
        }
        return false;                   /* 目标词是这行的前缀且更长 → 也越过了 */
    }
    return false;
}

esp_err_t srv_dict_lookup(const char *word, srv_dict_entry_t *out)
{
    if (word == NULL || word[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready && srv_dict_init() != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    return find_word(word, out) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* ===================== 词形还原 ===================== */

static void stem_push(char out[][40], int *n, const char *base, const char *input)
{
    if (*n >= STEM_MAX || base == NULL || base[0] == '\0') {
        return;
    }
    if (strcasecmp(base, input) == 0) {
        return;
    }
    for (int i = 0; i < *n; i++) {
        if (strcmp(out[i], base) == 0) {
            return;
        }
    }
    snprintf(out[*n], 40, "%s", base);
    (*n)++;
}

/**
 * @brief 生成词形还原候选（与 tools/dict_probe.py 的 stem_candidates 完全一致）
 *
 * 单靠规则不完善（brave 这种不规则变化靠表里自带的 lemma 字段），这里只处理
 * 大部分规则变化；顺序按「越可能的越靠前」，命中即停。
 */
static int make_stems(const char *w, char out[][40])
{
    const size_t len = strlen(w);
    int    n = 0;
    char   buf[48];

    if (len < 3 || len >= sizeof(buf)) {
        return 0;
    }

#define PUSH_BASE(cut, add)                                        \
    do {                                                           \
        size_t keep = len - (cut);                                 \
        if (keep + strlen(add) + 1 <= sizeof(buf)) {               \
            memcpy(buf, w, keep);                                  \
            buf[keep] = '\0';                                      \
            strncat(buf, add, sizeof(buf) - strlen(buf) - 1);      \
            stem_push(out, &n, buf, w);                            \
        }                                                          \
    } while (0)

    if (len > 4 && strcasecmp(w + len - 3, "ies") == 0) {
        PUSH_BASE(3, "y");
    }
    if (len > 4 && strcasecmp(w + len - 3, "ied") == 0) {
        PUSH_BASE(3, "y");
    }
    if (len > 3 && strcasecmp(w + len - 2, "es") == 0) {
        PUSH_BASE(2, "");
        PUSH_BASE(1, "");
    } else if (len > 3 && strcasecmp(w + len - 1, "s") == 0 &&
               strcasecmp(w + len - 2, "ss") != 0) {
        PUSH_BASE(1, "");
    }
    if (len > 3 && strcasecmp(w + len - 2, "ed") == 0) {
        PUSH_BASE(2, "");
        PUSH_BASE(1, "");
    }
    if (len > 4 && strcasecmp(w + len - 3, "ing") == 0) {
        PUSH_BASE(3, "");
        PUSH_BASE(3, "e");
    }
    if (len > 3 && strcasecmp(w + len - 2, "ly") == 0) {
        PUSH_BASE(2, "");
    }
    if (len > 3 && strcasecmp(w + len - 2, "er") == 0) {
        PUSH_BASE(2, "");
        PUSH_BASE(1, "");
    }
    if (len > 4 && strcasecmp(w + len - 3, "est") == 0) {
        PUSH_BASE(3, "");
        PUSH_BASE(2, "");
    }
#undef PUSH_BASE

    /* 辅音双写收缩（running→runn→run）；多轮处理 runnning 这种三连 */
    for (int round = 0; round < 3; round++) {
        bool grew = false;
        for (int i = 0, upto = n; i < upto && i < STEM_MAX; i++) {
            size_t l = strlen(out[i]);
            if (l > 2 && out[i][l - 1] == out[i][l - 2]) {
                char shorter[40];
                snprintf(shorter, sizeof(shorter), "%.*s", (int)(l - 1), out[i]);
                int before = n;
                stem_push(out, &n, shorter, w);
                if (n > before) {
                    grew = true;
                }
            }
        }
        if (!grew) {
            break;
        }
    }
    /* y→i 的（studies→studi→study） */
    for (int i = 0, upto = n; i < upto && i < STEM_MAX; i++) {
        size_t l = strlen(out[i]);
        if (l > 2 && out[i][l - 1] == 'i') {
            char fixed[40];
            snprintf(fixed, sizeof(fixed), "%.*sy", (int)(l - 1), out[i]);
            stem_push(out, &n, fixed, w);
        }
    }
    return n;
}

esp_err_t srv_dict_lookup_lemma(const char *word, srv_dict_entry_t *out)
{
    if (word == NULL || word[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready && srv_dict_init() != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    /* 1) 精确命中：直接用表里自带的原形 */
    if (find_word(word, out)) {
        return ESP_OK;
    }

    /* 2) 规则还原后再查：把「输入词 → 命中的词」记进 lemma，供界面提示 */
    static char stems[STEM_MAX][40];
    int n = make_stems(word, stems);
    for (int i = 0; i < n; i++) {
        if (find_word(stems[i], out)) {
            snprintf(out->lemma, sizeof(out->lemma), "%s", out->word);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/* ===================== 前缀联想 ===================== */

int srv_dict_prefix(const char *prefix, srv_dict_list_cb_t cb, void *arg, int max)
{
    if (prefix == NULL || prefix[0] == '\0' || cb == NULL) {
        return 0;
    }
    if (!s_ready && srv_dict_init() != ESP_OK) {
        return 0;
    }

    const int ai = anchor_of(prefix);
    if (!read_line_at(ai)) {
        return 0;
    }

    const size_t plen = strlen(prefix);
    char line[DICT_LINE_MAX];
    int  hits = 0;

    for (int i = 0; i < 4096; i++) {
        if (fgets(line, sizeof(line), s_fp) == NULL) {
            break;
        }
        int c = strncasecmp(line, prefix, plen);
        if (c != 0) {
            if (c > 0) {
                break;                  /* 已经走出前缀范围 */
            }
            continue;
        }
        memcpy(s_parse_buf, line, sizeof(s_parse_buf));
        srv_dict_entry_t e;
        if (!parse_entry(chomp(s_parse_buf), &e)) {
            continue;
        }
        if (!cb(&e, arg)) {
            break;
        }
        hits++;
        if (max > 0 && hits >= max) {
            break;
        }
    }
    return hits;
}

/* ===================== 拼写容错 ===================== */

static int pair_of(const char *w)
{
    if (w == NULL || w[0] == '\0' || w[1] == '\0') {
        return -1;
    }
    char a = (char)tolower((unsigned char)w[0]);
    char b = (char)tolower((unsigned char)w[1]);
    if (a < 'a' || a > 'z' || b < 'a' || b > 'z') {
        return -1;
    }
    return (a - 'a') * 26 + (b - 'a');
}

/* 编辑距离是否 ≤1（等长：改一个字母或相邻换位；差一个字符：增/删一个） */
static bool edit_dist_le1(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    if (a[0] == '\0' || b[0] == '\0') {
        return (la + lb) <= 1;
    }
    if (la == lb) {
        int diff = 0, first = -1;
        for (size_t i = 0; i < la; i++) {
            if (a[i] != b[i]) {
                if (diff == 0) {
                    first = (int)i;
                }
                diff++;
            }
        }
        if (diff == 1) {
            return true;
        }
        if (diff == 2 && a[first] == b[first + 1] && a[first + 1] == b[first]) {
            return true;                /* 相邻两个字母写反了 */
        }
        return false;
    }

    const char *s = (la < lb) ? a : b;      /* s 短 */
    const char *l = (la < lb) ? b : a;      /* l 长 */
    size_t ls = strlen(s), ll = strlen(l);
    size_t i = 0, j = 0;
    bool   skipped = false;
    while (i < ls && j < ll) {
        if (s[i] != l[j]) {
            if (skipped) {
                return false;
            }
            skipped = true;
            j++;
            continue;
        }
        i++;
        j++;
    }
    return true;
}

int srv_dict_suggest(const char *word, srv_dict_list_cb_t cb, void *arg, int max)
{
    if (word == NULL || cb == NULL || max <= 0) {
        return 0;
    }
    const int pair = pair_of(word);
    if (pair < 0) {
        return 0;
    }
    if (!s_ready && srv_dict_init() != ESP_OK) {
        return 0;
    }

    const size_t wlen = strlen(word);
    if (wlen >= SUGGEST_WORD_MAX) {
        return 0;
    }
    char word_lc[40];
    for (size_t i = 0; i <= wlen; i++) {
        word_lc[i] = (char)tolower((unsigned char)word[i]);
    }

    /* 扫描整个「前两字母」桶（最多读 64KB）：候选满了也不能停，
     * 否则像 aple→apple 这种常用词会被前面一堆冷门同形词挤掉 */
    int  n = 0;
    uint32_t read_bytes = 0;
    char line[DICT_LINE_MAX];
    char head[SUGGEST_WORD_MAX];
    static srv_dict_entry_t tmp;

    for (int ai = s_pair_from[pair];
         ai < s_pair_from[pair + 1] && ai < s_anchor_n && read_bytes <= SUGGEST_MAX_BYTES;
         ai++) {
        const uint32_t start = s_anchors[ai].off;
        const uint32_t end = (ai + 1 < s_anchor_n) ? s_anchors[ai + 1].off : s_main_size;
        read_bytes += (end > start) ? (end - start) : 0;

        if (!read_line_at(ai)) {
            continue;
        }
        for (int i = 0; i < s_step; i++) {
            if (fgets(line, sizeof(line), s_fp) == NULL) {
                break;
            }
            const char *tab = strchr(line, '\t');
            if (tab == NULL) {
                continue;
            }
            const size_t wl = (size_t)(tab - line);
            if (wl + 2 < wlen || wl > wlen + 2 || wl == 0 || wl >= SUGGEST_WORD_MAX) {
                continue;
            }
            memcpy(head, line, wl);             /* 先把词截出来单独比 */
            head[wl] = '\0';
            if (strcasecmp(head, word_lc) == 0) {
                continue;                       /* 等于输入词的不算建议 */
            }
            if (!edit_dist_le1(head, word_lc)) {
                continue;
            }

            /* 命中：解析整行拿徽章（常用词排序要用） */
            memcpy(s_parse_buf, line, sizeof(s_parse_buf));
            if (!parse_entry(chomp(s_parse_buf), &tmp)) {
                continue;
            }
            sug_item_t item;
            copy_word(item.word, sizeof(item.word), tmp.word);
            item.tags = tmp.tags;
            item.collins = tmp.collins;
            sug_insert(s_sug_items, &n, &item, (int)wlen);
        }
    }
    if (n == 0) {
        return 0;
    }

    /* 回调阶段再逐条解析出完整词条（命中过的一定在同一段里，很快） */
    static srv_dict_entry_t e;
    int hits = 0;
    for (int i = 0; i < n && hits < max; i++) {
        if (!find_word(s_sug_items[i].word, &e)) {
            continue;
        }
        if (!cb(&e, arg)) {
            break;
        }
        hits++;
    }
    return hits;
}

/* ===================== 逐词直译 ===================== */

void srv_dict_translate(const char *text, srv_dict_word_cb_t cb, void *arg)
{
    if (text == NULL || cb == NULL) {
        return;
    }

    static srv_dict_entry_t e;          /* 结构体较大，别放任务栈上 */
    const char *p = text;
    char tok[40];

    while (*p != '\0') {
        if (!isalpha((unsigned char)*p)) {
            p++;
            continue;
        }
        size_t n = 0;
        while (*p != '\0' && (isalpha((unsigned char)*p) || *p == '\'' || *p == '-')) {
            if (n < sizeof(tok) - 1) {
                tok[n++] = *p;
            }
            p++;
        }
        while (n > 0 && (tok[n - 1] == '\'' || tok[n - 1] == '-')) {
            n--;                        /* 去掉尾巴上的 ' 和 - */
        }
        tok[n] = '\0';
        if (n == 0) {
            continue;
        }

        if (srv_dict_lookup_lemma(tok, &e) == ESP_OK) {
            if (!cb(tok, &e, arg)) {
                return;
            }
        } else {
            if (!cb(tok, NULL, arg)) {
                return;
            }
        }
    }
}

/* ===================== 生命周期 ===================== */

/* 释放所有资源（保留路径） */
/* ===================== 词形变化表（variants.dat） =====================
 * 与主表分开一个文件：只存「原形 → 各种变形」，约 0.9MB，按原形排序 + 稀疏索引。
 * 查询流程：lookup_lemma 拿到原形 → 这里按原形块查找。
 */

#define VAR_LINE_MAX   160

typedef struct {
    uint32_t off;
    char     key[ANCHOR_KEY_BYTES + 1];
} var_anchor_t;

static char          s_path_var[80];
static FILE         *s_var_fp = NULL;
static var_anchor_t *s_var_anchors = NULL;
static int           s_var_anchor_n = 0;
static int           s_var_step = 64;
static int           s_var_count = 0;
static uint32_t      s_var_size = 0;
static bool          s_var_ready = false;
static bool          s_var_tried = false;   /*!< 找过一次就不再反复开文件 */
static char          s_var_line[VAR_LINE_MAX];

static void var_close(void)
{
    if (s_var_fp != NULL) {
        fclose(s_var_fp);
        s_var_fp = NULL;
    }
    free(s_var_anchors);
    s_var_anchors = NULL;
    s_var_anchor_n = 0;
    s_var_count = 0;
    s_var_size = 0;
    s_var_ready = false;
    s_var_tried = false;
}

static bool var_open(void)
{
    if (s_var_ready) {
        return true;
    }
    if (!srv_sd_is_mounted()) {
        return false;
    }

    /* 文件可能后拷进来：失败不锁死，下次查询再试；只有第一次打日志 */
    const bool first_try = !s_var_tried;
    s_var_tried = true;

    const char *mp = srv_sd_mount_point();
    if (s_path_var[0] == '\0') {
        snprintf(s_path_var, sizeof(s_path_var), "%s/dict/variants.dat", mp);
    }
    char idx_path[80];
    snprintf(idx_path, sizeof(idx_path), "%s/dict/variants.idx", mp);

    FILE *fi = fopen(idx_path, "rb");
    if (fi == NULL) {
        if (first_try) {
            ESP_LOGW(TAG, "没有变形表（%s），用 tools/build_variants.py 生成", idx_path);
        }
        return false;
    }

    uint8_t hdr[24];
    if (fread(hdr, 1, sizeof(hdr), fi) != sizeof(hdr) || memcmp(hdr, "DCTV", 4) != 0) {
        ESP_LOGE(TAG, "变形表索引损坏或版本不对: %s", idx_path);
        fclose(fi);
        return false;
    }
    uint16_t ver = 0, step = 0;
    memcpy(&ver, hdr + 4, 2);
    memcpy(&step, hdr + 6, 2);
    memcpy(&s_var_count, hdr + 8, 4);
    memcpy(&s_var_size, hdr + 12, 4);
    if (ver != 1 || step < 8 || step > 256 || s_var_count <= 0) {
        ESP_LOGE(TAG, "变形表索引头异常：ver=%u step=%u count=%d", ver, step, s_var_count);
        fclose(fi);
        return false;
    }
    s_var_step = step;

    const int n = (s_var_count + s_var_step - 1) / s_var_step;
    s_var_anchors = heap_caps_malloc((size_t)n * sizeof(var_anchor_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_var_anchors == NULL) {
        s_var_anchors = malloc((size_t)n * sizeof(var_anchor_t));
    }
    if (s_var_anchors == NULL) {
        ESP_LOGE(TAG, "变形表索引内存不足（%d 个锚点）", n);
        fclose(fi);
        return false;
    }

    for (int i = 0; i < n; i++) {
        uint8_t raw[12];
        if (fread(raw, 1, sizeof(raw), fi) != sizeof(raw)) {
            ESP_LOGE(TAG, "变形表索引不完整（第 %d/%d 个锚点）", i, n);
            fclose(fi);
            free(s_var_anchors);
            s_var_anchors = NULL;
            return false;
        }
        memcpy(&s_var_anchors[i].off, raw, 4);
        memcpy(s_var_anchors[i].key, raw + 4, ANCHOR_KEY_BYTES);
        s_var_anchors[i].key[ANCHOR_KEY_BYTES] = '\0';
    }
    fclose(fi);
    s_var_anchor_n = n;

    s_var_fp = fopen(s_path_var, "rb");
    if (s_var_fp == NULL) {
        ESP_LOGW(TAG, "打不开变形表 %s", s_path_var);
        free(s_var_anchors);
        s_var_anchors = NULL;
        s_var_anchor_n = 0;
        return false;
    }

    s_var_ready = true;
    ESP_LOGI(TAG, "变形表就绪：%d 个原形（%d 个锚点）", s_var_count, s_var_anchor_n);
    return true;
}

bool srv_dict_variants_ready(void)
{
    return s_var_ready;
}

/* 解析 `p:ran|d:run|i:running|3:runs` 到结构体（值里可能带逗号分隔的多个形式） */
static void parse_forms(char *forms, srv_dict_forms_t *out)
{
    chomp(forms);

    char *p = forms;
    while (p != NULL && *p != '\0') {
        char *bar = strchr(p, '|');
        if (bar != NULL) {
            *bar = '\0';
        }
        if (p[0] != '\0' && p[1] == ':') {
            char *val = p + 2;
            char *dst = NULL;
            switch (p[0]) {
            case 'p': dst = out->past;      break;
            case 'd': dst = out->past_part; break;
            case 'i': dst = out->ing;       break;
            case '3': dst = out->third;     break;
            case 's': dst = out->plural;    break;
            case 'r': dst = out->comp;      break;
            case 't': dst = out->sup;       break;
            default:  break;
            }
            if (dst != NULL) {
                snprintf(dst, 64, "%s", val);
            }
        }
        p = (bar != NULL) ? bar + 1 : NULL;
    }
}

static bool var_find(const char *base, srv_dict_forms_t *out)
{
    if (!var_open()) {
        return false;
    }

    char tk[ANCHOR_KEY_BYTES];
    key_of(base, tk);

    int lo = 0, hi = s_var_anchor_n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (memcmp(s_var_anchors[mid].key, tk, ANCHOR_KEY_BYTES) <= 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    const int ai = (lo > 0) ? (lo - 1) : 0;

    if (fseek(s_var_fp, (long)s_var_anchors[ai].off, SEEK_SET) != 0) {
        return false;
    }

    /* 锚点 key 只有 8 字节，目标词可能落在下一块，所以最多连读两块 */
    for (int i = 0; i < 2 * s_var_step; i++) {
        if (fgets(s_var_line, sizeof(s_var_line), s_var_fp) == NULL) {
            return false;
        }
        char *tab = strchr(s_var_line, '\t');
        if (tab == NULL) {
            continue;
        }
        *tab = '\0';
        const int c = strcmp(s_var_line, base);
        if (c == 0) {
            parse_forms(tab + 1, out);
            return true;
        }
        if (c > 0) {
            return false;
        }
    }
    return false;
}

esp_err_t srv_dict_forms(const char *word, srv_dict_forms_t *out)
{
    if (word == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    char w[40];
    snprintf(w, sizeof(w), "%s", word);
    for (char *p = w; *p != '\0'; p++) {
        *p = (char)tolower((unsigned char)*p);
    }

    srv_dict_entry_t e;
    memset(&e, 0, sizeof(e));
    const char *base = w;
    if (srv_dict_lookup_lemma(w, &e) == ESP_OK) {
        base = (e.lemma[0] != '\0') ? e.lemma : e.word;
    }
    snprintf(out->base, sizeof(out->base), "%s", base);
    out->changed = (strcmp(base, w) != 0);

    const bool hit = var_find(base, out);
    out->found = hit;

    return (hit || out->changed) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static void dict_close(void)
{
    var_close();
    if (s_fp != NULL) {
        fclose(s_fp);
        s_fp = NULL;
    }
    free(s_anchors);
    s_anchors = NULL;
    free(s_pair_from);
    s_pair_from = NULL;
    s_anchor_n = 0;
    s_count = 0;
    s_main_size = 0;
    s_ready = false;
}

void srv_dict_reload(void)
{
    dict_close();
    (void)srv_dict_init();
}

/* 由锚点表推导两字母桶的起止下标 */
static void build_pairs(void)
{
    int ai = 0;
    for (int pair = 0; pair < PAIR_COUNT; pair++) {
        const char c0 = (char)('a' + pair / 26);
        const char c1 = (char)('a' + pair % 26);
        while (ai < s_anchor_n) {
            const char k0 = s_anchors[ai].key[0];
            const char k1 = s_anchors[ai].key[1];
            bool ge = (k0 != c0) ? (k0 > c0) : (k1 >= c1);
            if (ge) {
                break;
            }
            ai++;
        }
        s_pair_from[pair] = (uint16_t)ai;
    }
    s_pair_from[PAIR_COUNT] = (uint16_t)s_anchor_n;
}

esp_err_t srv_dict_init(void)
{
    if (s_ready) {
        /* SD 重挂之后文件句柄会失效：读一眼首行自检，不行就重来。
         * 注意锚点 key 是 NUL 填充的词前缀，只能比「它自己的长度」那么多字节 */
        char   line[64];
        size_t klen = 0;
        while (klen < ANCHOR_KEY_BYTES && s_anchors[0].key[klen] != '\0') {
            klen++;
        }
        if (klen > 0 && s_fp != NULL && fseek(s_fp, 0, SEEK_SET) == 0 &&
            fgets(line, sizeof(line), s_fp) != NULL &&
            memcmp(line, s_anchors[0].key, klen) == 0) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "词库句柄失效，重新加载");
        dict_close();
    }

    if (!srv_sd_is_mounted()) {
        ESP_LOGW(TAG, "SD 未挂载，词典不可用");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_path_dat[0] == '\0') {
        (void)srv_dict_path();
        snprintf(s_path_idx, sizeof(s_path_idx), "%s/dict/dict.idx", srv_sd_mount_point());
    }

    /* ---- 读索引 ---- */
    FILE *fi = fopen(s_path_idx, "rb");
    if (fi == NULL) {
        ESP_LOGW(TAG, "没有索引文件 %s（词库要用 tools/build_dict.py 重新生成）", s_path_idx);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t hdr[24];
    if (fread(hdr, 1, sizeof(hdr), fi) != sizeof(hdr) || memcmp(hdr, "DCTX", 4) != 0) {
        ESP_LOGE(TAG, "索引文件损坏或版本不对: %s", s_path_idx);
        fclose(fi);
        return ESP_ERR_INVALID_SIZE;
    }
    uint16_t ver = 0, step = 0;
    memcpy(&ver, hdr + 4, 2);
    memcpy(&step, hdr + 6, 2);
    memcpy(&s_count, hdr + 8, 4);
    memcpy(&s_main_size, hdr + 12, 4);
    if (ver != 1 || step < 8 || step > 256 || s_count <= 0) {
        ESP_LOGE(TAG, "索引头异常：ver=%u step=%u count=%d", ver, step, s_count);
        fclose(fi);
        return ESP_ERR_INVALID_SIZE;
    }
    s_step = step;

    const int n = (s_count + s_step - 1) / s_step;
    s_anchors = heap_caps_malloc((size_t)n * sizeof(dict_anchor_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_anchors == NULL) {
        s_anchors = malloc((size_t)n * sizeof(dict_anchor_t));
    }
    s_pair_from = heap_caps_malloc((PAIR_COUNT + 1) * sizeof(uint16_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_pair_from == NULL) {
        s_pair_from = malloc((PAIR_COUNT + 1) * sizeof(uint16_t));
    }
    if (s_anchors == NULL || s_pair_from == NULL) {
        ESP_LOGE(TAG, "索引内存不足（%d 个锚点）", n);
        fclose(fi);
        dict_close();
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < n; i++) {
        uint8_t raw[12];
        if (fread(raw, 1, sizeof(raw), fi) != sizeof(raw)) {
            ESP_LOGE(TAG, "索引文件不完整（读到第 %d/%d 个锚点）", i, n);
            fclose(fi);
            dict_close();
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(&s_anchors[i].off, raw, 4);
        memcpy(s_anchors[i].key, raw + 4, ANCHOR_KEY_BYTES);
        s_anchors[i].key[ANCHOR_KEY_BYTES] = '\0';
    }
    fclose(fi);
    s_anchor_n = n;
    build_pairs();

    /* ---- 打开主表并做个格式自检 ---- */
    s_fp = fopen(s_path_dat, "rb");
    if (s_fp == NULL) {
        ESP_LOGW(TAG, "没有词库主表 %s", s_path_dat);
        dict_close();
        return ESP_ERR_NOT_FOUND;
    }
    char line[DICT_LINE_MAX];
    if (fgets(line, sizeof(line), s_fp) == NULL) {
        ESP_LOGW(TAG, "词库主表是空的: %s", s_path_dat);
        dict_close();
        return ESP_ERR_INVALID_SIZE;
    }
    int tabs = 0;
    for (const char *q = line; *q != '\0'; q++) {
        if (*q == '\t') {
            tabs++;
        }
    }
    if (tabs != 4) {
        ESP_LOGE(TAG, "词库格式不对（首行 %d 个 TAB，应是 4 个）——"
                      "请用 tools/build_dict.py 重新生成 dict.dat/dict.idx", tabs);
        dict_close();
        return ESP_ERR_INVALID_SIZE;
    }

    s_ready = true;
    ESP_LOGI(TAG, "词库就绪：%d 条，%u 字节，%d 个锚点（step=%d，常驻 %u 字节）",
             s_count, (unsigned)s_main_size, s_anchor_n, s_step,
             (unsigned)(s_anchor_n * sizeof(dict_anchor_t) + (PAIR_COUNT + 1) * 2));
    return ESP_OK;
}