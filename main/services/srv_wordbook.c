#include "srv_wordbook.h"

#include "srv_sd.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "srv_wb";

#define WORDS_DIR   "words"
#define PROG_NAME   "progress.dat"
#define PROG_MAX    8192

/* ===================== 词书结构 ===================== */

typedef struct {
    char name[WB_NAME_MAX];
    int  unit_nos[WB_MAX_UNITS];    /*!< unit-XX.txt 里的 XX */
    int  unit_cnt;
} wb_book_t;

static wb_book_t s_books[WB_MAX_BOOKS];
static int       s_book_n = 0;
static bool      s_ready = false;
static bool      s_scanned = false;

/* ===================== 进度 ===================== */

typedef struct {
    char word[WB_NAME_MAX];
    int8_t  score;      /*!< 0~5 */
    uint16_t wrong;     /*!< 答错次数 */
} wb_prog_t;

static wb_prog_t *s_prog = NULL;
static int        s_prog_n = 0;
static int        s_prog_cap = 0;
static bool       s_prog_loaded = false;
static int        s_unsaved = 0;    /*!< 距离上次落盘的改动数 */

/* ===================== 会话 ===================== */

static char s_sess[WB_UNIT_WORD_MAX][WB_NAME_MAX];
static int  s_sess_n = 0;
static int  s_sess_pos = 0;
static int  s_known = 0;
static int  s_unknown = 0;
static bool s_session = false;

/* ===================== 小工具 ===================== */

static void path_join(char *dst, size_t cap, const char *a, const char *b, const char *c)
{
    /* 用 %.*s 限长：d_name 最长 255 字节，直接 %s 会被 -Werror=format-truncation 拦 */
    const int n = (int)(cap / 3);
    snprintf(dst, cap, "%.*s/%.*s/%.*s",
             n, a, n, b, n, (c != NULL) ? c : "");
}

static const char *words_root(void)
{
    static char root[80];
    if (root[0] == '\0') {
        snprintf(root, sizeof(root), "%s/%s", srv_sd_mount_point(), WORDS_DIR);
    }
    return root;
}

static void prog_free(void)
{
    free(s_prog);
    s_prog = NULL;
    s_prog_n = 0;
    s_prog_cap = 0;
    s_prog_loaded = false;
}

static int prog_cmp(const void *a, const void *b)
{
    return strcmp(((const wb_prog_t *)a)->word, ((const wb_prog_t *)b)->word);
}

static void prog_load(void)
{
    if (s_prog_loaded) {
        return;
    }
    s_prog_loaded = true;
    if (!srv_sd_is_mounted()) {
        return;
    }

    char path[96];
    path_join(path, sizeof(path), srv_sd_mount_point(), WORDS_DIR, PROG_NAME);
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return;                     /* 第一次学习，还没有进度 */
    }

    char line[80];
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *tab1 = strchr(line, '\t');
        if (tab1 == NULL) {
            continue;
        }
        *tab1 = '\0';
        int score = 0;
        int wrong = 0;
        (void)sscanf(tab1 + 1, "%d\t%d", &score, &wrong);
        if (line[0] == '\0') {
            continue;
        }

        if (s_prog_n >= s_prog_cap) {
            int cap = (s_prog_cap == 0) ? 512 : s_prog_cap * 2;
            if (cap > PROG_MAX) {
                cap = PROG_MAX;
            }
            if (cap <= s_prog_cap) {
                break;
            }
            wb_prog_t *np = heap_caps_realloc(s_prog, (size_t)cap * sizeof(wb_prog_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (np == NULL) {
                np = realloc(s_prog, (size_t)cap * sizeof(wb_prog_t));
            }
            if (np == NULL) {
                break;
            }
            s_prog = np;
            s_prog_cap = cap;
        }

        wb_prog_t *p = &s_prog[s_prog_n++];
        snprintf(p->word, sizeof(p->word), "%.*s", (int)(sizeof(p->word) - 1), line);
        p->score = (int8_t)((score < 0) ? 0 : (score > 5 ? 5 : score));
        p->wrong = (uint16_t)((wrong < 0) ? 0 : (wrong > 65535 ? 65535 : wrong));
    }
    fclose(fp);

    if (s_prog_n > 1) {
        qsort(s_prog, (size_t)s_prog_n, sizeof(wb_prog_t), prog_cmp);
    }
    ESP_LOGI(TAG, "学习进度 %d 个词", s_prog_n);
}

static wb_prog_t *prog_find(const char *word)
{
    if (s_prog == NULL || s_prog_n == 0) {
        return NULL;
    }
    int lo = 0, hi = s_prog_n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(s_prog[mid].word, word);
        if (c == 0) {
            return &s_prog[mid];
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

static wb_prog_t *prog_get(const char *word, bool create)
{
    wb_prog_t *p = prog_find(word);
    if (p != NULL || !create) {
        return p;
    }
    if (s_prog_n >= PROG_MAX) {
        return NULL;
    }
    if (s_prog_n >= s_prog_cap) {
        int cap = (s_prog_cap == 0) ? 512 : s_prog_cap * 2;
        if (cap > PROG_MAX) {
            cap = PROG_MAX;
        }
        wb_prog_t *np = heap_caps_realloc(s_prog, (size_t)cap * sizeof(wb_prog_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (np == NULL) {
            np = realloc(s_prog, (size_t)cap * sizeof(wb_prog_t));
        }
        if (np == NULL) {
            return NULL;
        }
        s_prog = np;
        s_prog_cap = cap;
    }

    /* 保持按词排序：插入到正确位置（数量不大，memmove 成本可忽略） */
    int pos = s_prog_n;
    while (pos > 0 && strcmp(s_prog[pos - 1].word, word) > 0) {
        pos--;
    }
    memmove(&s_prog[pos + 1], &s_prog[pos], (size_t)(s_prog_n - pos) * sizeof(wb_prog_t));
    s_prog_n++;

    p = &s_prog[pos];
    memset(p, 0, sizeof(*p));
    snprintf(p->word, sizeof(p->word), "%s", word);
    return p;
}

static void prog_save(void)
{
    if (!srv_sd_is_mounted()) {
        return;
    }

    char path[96];
    path_join(path, sizeof(path), srv_sd_mount_point(), WORDS_DIR, PROG_NAME);
    FILE *fp = fopen(path, "w");
    if (fp == NULL) {
        ESP_LOGW(TAG, "写进度失败: %s", path);
        return;
    }
    for (int i = 0; i < s_prog_n; i++) {
        fprintf(fp, "%s\t%d\t%u\n", s_prog[i].word, (int)s_prog[i].score,
                (unsigned)s_prog[i].wrong);
    }
    fclose(fp);
    s_unsaved = 0;
}

/* ===================== 词书扫描 ===================== */

static int unit_no_of(const char *name)
{
    int n = 0;
    if (sscanf(name, "unit-%d.txt", &n) == 1) {
        return n;
    }
    return -1;
}

esp_err_t srv_wordbook_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (!srv_sd_is_mounted()) {
        ESP_LOGW(TAG, "SD 未挂载，单词学习不可用");
        return ESP_ERR_INVALID_STATE;
    }

    s_book_n = 0;
    const char *root = words_root();
    DIR *d = opendir(root);
    if (d == NULL) {
        ESP_LOGW(TAG, "没有 /words 目录");
        s_ready = true;
        s_scanned = true;
        return ESP_OK;
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_book_n < WB_MAX_BOOKS) {
        if (e->d_name[0] == '.') {
            continue;
        }
        char sub[128];
        path_join(sub, sizeof(sub), root, e->d_name, NULL);
        DIR *sd = opendir(sub);
        if (sd == NULL) {
            continue;               /* 不是目录 */
        }

        wb_book_t *bk = &s_books[s_book_n];
        memset(bk, 0, sizeof(*bk));
        snprintf(bk->name, sizeof(bk->name), "%.*s",
                 (int)(sizeof(bk->name) - 1), e->d_name);

        struct dirent *ue;
        while ((ue = readdir(sd)) != NULL && bk->unit_cnt < WB_MAX_UNITS) {
            int no = unit_no_of(ue->d_name);
            if (no > 0) {
                bk->unit_nos[bk->unit_cnt++] = no;
            }
        }
        closedir(sd);

        if (bk->unit_cnt == 0) {
            continue;               /* 空目录不算词书 */
        }
        /* 单元按编号排序（readdir 顺序不可靠） */
        for (int i = 1; i < bk->unit_cnt; i++) {
            int v = bk->unit_nos[i];
            int j = i - 1;
            while (j >= 0 && bk->unit_nos[j] > v) {
                bk->unit_nos[j + 1] = bk->unit_nos[j];
                j--;
            }
            bk->unit_nos[j + 1] = v;
        }
        s_book_n++;
    }
    closedir(d);

    prog_load();
    s_ready = true;
    s_scanned = true;
    ESP_LOGI(TAG, "词书 %d 本（进度 %d 词）", s_book_n, s_prog_n);
    return ESP_OK;
}

void srv_wordbook_reload(void)
{
    s_ready = false;
    s_scanned = false;
    prog_free();
    memset(s_books, 0, sizeof(s_books));
    s_book_n = 0;
    (void)srv_wordbook_init();
}

bool srv_wordbook_ready(void)
{
    return s_ready;
}

int srv_wordbook_book_count(void)
{
    return s_book_n;
}

const char *srv_wordbook_book_name(int book)
{
    if (book < 0 || book >= s_book_n) {
        return "";
    }
    return s_books[book].name;
}

int srv_wordbook_unit_count(int book)
{
    if (book < 0 || book >= s_book_n) {
        return 0;
    }
    return s_books[book].unit_cnt;
}

const char *srv_wordbook_unit_name(int book, int unit)
{
    static char buf[32];
    if (unit < 0 || unit >= srv_wordbook_unit_count(book)) {
        return "";
    }
    snprintf(buf, sizeof(buf), "Unit %02d", s_books[book].unit_nos[unit]);
    return buf;
}

int srv_wordbook_unit_words(int book, int unit)
{
    if (unit < 0 || unit >= srv_wordbook_unit_count(book)) {
        return 0;
    }
    char path[160];
    snprintf(path, sizeof(path), "%s/%s/unit-%02d.txt", words_root(),
             s_books[book].name, s_books[book].unit_nos[unit]);
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        return 0;
    }
    int n = 0;
    char line[WB_NAME_MAX + 4];
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (line[0] != '\n' && line[0] != '\0') {
            n++;
        }
    }
    fclose(fp);
    return n;
}

int srv_wordbook_wrong_count(void)
{
    prog_load();
    int n = 0;
    for (int i = 0; i < s_prog_n; i++) {
        if (s_prog[i].wrong > 0) {
            n++;
        }
    }
    return n;
}

/* ===================== 抽词 ===================== */

/* 候选池：词 + 权重（权重大的多放几份）。放 PSRAM，按需扩容，
 * 不能做成静态数组——4096 × 40B 会把内部 RAM 吃掉 160KB。 */
#define POOL_MAX 4096

static char (*s_pool)[WB_NAME_MAX] = NULL;
static int  s_pool_n = 0;
static int  s_pool_cap = 0;

static bool pool_reserve(int need)
{
    if (need <= s_pool_cap) {
        return true;
    }
    int cap = (s_pool_cap == 0) ? 256 : s_pool_cap;
    while (cap < need && cap < POOL_MAX) {
        cap *= 2;
    }
    if (cap > POOL_MAX) {
        cap = POOL_MAX;
    }
    if (cap < need) {
        return false;
    }
    char (*np)[WB_NAME_MAX] = heap_caps_realloc(s_pool, (size_t)cap * WB_NAME_MAX,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (np == NULL) {
        np = realloc(s_pool, (size_t)cap * WB_NAME_MAX);
    }
    if (np == NULL) {
        return false;
    }
    s_pool = np;
    s_pool_cap = cap;
    return true;
}

static void pool_add(const char *word, int weight)
{
    if (weight < 1) {
        weight = 1;
    }
    if (s_pool_n + weight > POOL_MAX) {
        weight = POOL_MAX - s_pool_n;
    }
    if (weight <= 0 || !pool_reserve(s_pool_n + weight)) {
        return;
    }
    for (int i = 0; i < weight; i++) {
        snprintf(s_pool[s_pool_n], WB_NAME_MAX, "%s", word);
        s_pool_n++;
    }
}

static bool sess_has(const char *word)
{
    for (int i = 0; i < s_sess_n; i++) {
        if (strcmp(s_sess[i], word) == 0) {
            return true;
        }
    }
    return false;
}

static void sess_reset(void)
{
    s_sess_n = 0;
    s_sess_pos = 0;
    s_known = 0;
    s_unknown = 0;
    s_pool_n = 0;
    s_session = false;
}

static void sess_shuffle_pick(int count)
{
    if (count > WB_UNIT_WORD_MAX) {
        count = WB_UNIT_WORD_MAX;
    }
    if (count > s_pool_n) {
        count = s_pool_n;
    }

    /* 从带重复权重的池子里随机抽，去重后进会话 */
    int guard = 0;
    while (s_sess_n < count && s_pool_n > 0 && guard < s_pool_n * 4) {
        guard++;
        const int r = (int)(esp_random() % (uint32_t)s_pool_n);
        const char *w = s_pool[r];
        if (!sess_has(w)) {
            snprintf(s_sess[s_sess_n], sizeof(s_sess[0]), "%s", w);
            s_sess_n++;
        }
    }

    /* 池子里去重后不够，就把池子里没进去的词按顺序补上 */
    if (s_sess_n < count) {
        for (int i = 0; i < s_pool_n && s_sess_n < count; i++) {
            if (!sess_has(s_pool[i])) {
                snprintf(s_sess[s_sess_n], sizeof(s_sess[0]), "%s", s_pool[i]);
                s_sess_n++;
            }
        }
    }

    s_session = (s_sess_n > 0);
    ESP_LOGI(TAG, "开始学习：%d 个词（池子 %d）", s_sess_n, s_pool_n);
}

esp_err_t srv_wordbook_start_units(int book, const bool *unit_sel, int count)
{
    if (count <= 0) {
        count = 20;
    }
    if (srv_wordbook_init() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (book < 0 || book >= s_book_n || unit_sel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    sess_reset();
    prog_load();

    char path[160];
    char line[WB_NAME_MAX + 4];

    for (int u = 0; u < s_books[book].unit_cnt; u++) {
        if (!unit_sel[u]) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s/unit-%02d.txt", words_root(),
                 s_books[book].name, s_books[book].unit_nos[u]);
        FILE *fp = fopen(path, "r");
        if (fp == NULL) {
            continue;
        }
        while (fgets(line, sizeof(line), fp) != NULL) {
            /* 去掉行尾 */
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                line[--len] = '\0';
            }
            if (len == 0) {
                continue;
            }
            for (size_t i = 0; i < len; i++) {
                if (line[i] >= 'A' && line[i] <= 'Z') {
                    line[i] = (char)(line[i] + 32);
                }
            }

            const wb_prog_t *p = prog_find(line);
            int weight = 3;                       /* 没学过 */
            if (p != NULL) {
                if (p->score <= 1) {
                    weight = 2;
                } else if (p->score >= 4) {
                    weight = 1;
                }
                if (p->wrong > 0) {
                    weight += 2;                  /* 错词优先 */
                }
            }
            pool_add(line, weight);
        }
        fclose(fp);
    }

    sess_shuffle_pick(count);
    return s_session ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t srv_wordbook_start_wrong(int count)
{
    if (count <= 0) {
        count = 20;
    }
    if (srv_wordbook_init() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    sess_reset();
    prog_load();

    for (int i = 0; i < s_prog_n; i++) {
        if (s_prog[i].wrong > 0) {
            int w = 2 + ((s_prog[i].wrong > 3) ? 2 : 0);
            pool_add(s_prog[i].word, w);
        }
    }

    sess_shuffle_pick(count);
    return s_session ? ESP_OK : ESP_ERR_NOT_FOUND;
}

bool srv_wordbook_session_active(void)
{
    return s_session;
}

int srv_wordbook_session_total(void)
{
    return s_sess_n;
}

int srv_wordbook_session_done(void)
{
    return s_sess_pos;
}

int srv_wordbook_session_known(void)
{
    return s_known;
}

const char *srv_wordbook_current(void)
{
    if (!s_session || s_sess_pos >= s_sess_n) {
        return "";
    }
    return s_sess[s_sess_pos];
}

void srv_wordbook_answer(bool known)
{
    if (!s_session || s_sess_pos >= s_sess_n) {
        return;
    }

    wb_prog_t *p = prog_get(s_sess[s_sess_pos], true);
    if (p != NULL) {
        if (known) {
            if (p->score < 5) {
                p->score++;
            }
            /* 认识了就移出错词本（wrong 归零）；下次再错会重新进来 */
            p->wrong = 0;
            s_known++;
        } else {
            if (p->score > 0) {
                p->score--;
            }
            if (p->wrong < 65535) {
                p->wrong++;
            }
            s_unknown++;
        }
        s_unsaved++;
        if (s_unsaved >= 10) {
            prog_save();
        }
    }

    s_sess_pos++;
    if (s_sess_pos >= s_sess_n) {
        prog_save();
        s_session = false;
        ESP_LOGI(TAG, "学习结束：认识 %d / 不认识 %d", s_known, s_unknown);
    }
}

void srv_wordbook_stop(void)
{
    if (s_session) {
        prog_save();
    }
    sess_reset();
    /* 池子按需扩容的，退出会话就还回去（最多 160KB PSRAM） */
    free(s_pool);
    s_pool = NULL;
    s_pool_cap = 0;
}
