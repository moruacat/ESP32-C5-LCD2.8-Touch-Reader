#include "ui_words.h"

#include "ui_common.h"
#include "ui_dialog.h"
#include "ui_icons.h"
#include "ui_ime.h"     /* 查词要能打中文，走拼音输入层 */
#include "app_navigator.h"
#include "srv_dict.h"
#include "srv_dict_zh.h"
#include "srv_sd.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ===================== 布局 ===================== */
#define SEARCH_X     8
#define SEARCH_Y     (UI_TITLEBAR_H + 4)
/* 搜索框右侧让出两个小按钮：「变形」「学习」 */
#define ENTRY_W      48
#define ENTRY_H      32
#define ENTRY_STUDY_X (ui_scr_w() - SEARCH_X - ENTRY_W)
#define ENTRY_FORM_X  (ENTRY_STUDY_X - ENTRY_W - 4)
#define SEARCH_W     (ENTRY_FORM_X - SEARCH_X - 4)
#define SEARCH_H     32

#define BODY_X       8
#define BODY_Y       (SEARCH_Y + SEARCH_H + 6)
#define BODY_W       (ui_scr_w() - BODY_X * 2)
#define BODY_H       (ui_scr_h() - BODY_Y - 4)

#define ROW_H        36
#define ROW_GAP      4
#define LIST_MAX     40          /*!< 联想列表最多显示多少条 */

#define WORD_LEN     32          /*!< 查词模式输入上限 */
#define SENT_LEN     120         /*!< 直译模式输入上限（键盘最多 128） */
#define SUGGEST_MAX  3           /*!< 拼写容错最多列几条 */

/* 标题栏右侧两个模式按钮（旋转键在返回键右侧，不占这里的位置） */
#define TAB_W        44
#define TAB_H        20
#define TAB_TRANS_X  (ui_scr_w() - TAB_W - 8)
#define TAB_WORD_X   (TAB_TRANS_X - TAB_W - 4)

/* ===================== 页面状态 ===================== */

static lv_obj_t *s_owner;        /*!< 页面根容器：销毁回调靠它区分新旧页面 */
static lv_obj_t *s_search_lb;    /*!< 搜索框里的文字 */
static lv_obj_t *s_tab_word;     /*!< 模式按钮：查词 */
static lv_obj_t *s_tab_trans;    /*!< 模式按钮：译 */
static lv_obj_t *s_list;         /*!< 联想列表（滚动容器） */
static lv_obj_t *s_detail;       /*!< 释义详情（滚动容器） */
static lv_obj_t *s_detail_word;
static lv_obj_t *s_detail_ph;
static lv_obj_t *s_detail_def;
static lv_obj_t *s_detail_badge;
static lv_obj_t *s_detail_lemma; /*!< 「→ run 查看」，可点 */
static lv_obj_t *s_back_btn;     /*!< 详情视图里的「返回」 */
static lv_obj_t *s_trans;        /*!< 直译视图（滚动容器） */
static lv_obj_t *s_rel;          /*!< 详情底部的「相关短语」容器（常量见下方「相关短语」节） */
static char      s_rel_skip[WORD_LEN];        /*!< 当前词，列短语时排除自身 */

static int  s_mode = 0;          /*!< 0=查词 1=译 */
static char s_word[WORD_LEN];    /*!< 当前查询词 */
static char s_sent[SENT_LEN];    /*!< 当前直译的句子 */
static char s_detail_word_txt[WORD_LEN];  /*!< 详情里当前显示的词（返回时用） */
static char s_lemma_txt[WORD_LEN];        /*!< 详情里的原形提示词 */
static bool s_detail_from_trans; /*!< 详情是从直译视图点进来的 → 返回回直译 */
static char s_sug_text[SUGGEST_MAX][WORD_LEN];

/*! 列表下一行的 y（各 fill 函数用它排行） */
static lv_coord_t s_row_y;

/* 页面销毁时把所有 static 置空。
 * 必须比对 s_owner：切页用的是 lv_obj_delete_async，手快「A→B→A」时新页面
 * 已经建好，旧页面的回调无脑清空会把新页面的指针一起清掉。 */
static void words_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    if (lv_event_get_target(e) != s_owner) {
        return;
    }
    s_owner        = NULL;
    s_search_lb    = NULL;
    s_tab_word     = NULL;
    s_tab_trans    = NULL;
    s_list         = NULL;
    s_detail       = NULL;
    s_detail_word  = NULL;
    s_detail_ph    = NULL;
    s_detail_def   = NULL;
    s_detail_badge = NULL;
    s_detail_lemma = NULL;
    s_rel          = NULL;
    s_back_btn     = NULL;
    s_trans        = NULL;
    s_rel_skip[0]   = '\0';
    s_word[0]       = '\0';
    s_sent[0]       = '\0';
    s_lemma_txt[0]  = '\0';
    s_detail_word_txt[0] = '\0';
    s_detail_from_trans  = false;
    s_row_y = 0;
}

/* 定长拷贝：不用 snprintf，免得编译器报 format-truncation（IDF 默认 -Werror） */
static void str_copy_trunc(char *dst, size_t cap, const char *src)
{
    if (cap == 0) {
        return;
    }
    size_t n = strlen(src);
    if (n > cap - 1) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ===================== 视图切换 ===================== */

enum { VIEW_LIST = 0, VIEW_DETAIL, VIEW_TRANS };

static void view_show(int view)
{
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_trans, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);

    if (view == VIEW_DETAIL) {
        lv_obj_remove_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);
    } else if (view == VIEW_TRANS) {
        lv_obj_remove_flag(s_trans, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 列表里的一句提示（清空列表后建，跟着列表一起被清掉） */
static void list_hint(const char *text)
{
    lv_obj_clean(s_list);

    lv_obj_t *lb = lv_label_create(s_list);
    lv_obj_set_style_text_color(lb, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lb, &font_ui_14, 0);
    lv_label_set_text(lb, text);
    lv_obj_set_width(lb, BODY_W - 16);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lb, LV_ALIGN_CENTER, 0, 0);
}

/* ===================== 释义详情 ===================== */

/* 徽章文字："四级 考研 ★★★" */
static void build_badges(const srv_dict_entry_t *e, char *buf, size_t cap)
{
    static const struct {
        uint16_t bit;
        const char *txt;
    } MAP[] = {
        { SRV_DICT_TAG_CET4,   "四级" },
        { SRV_DICT_TAG_CET6,   "六级" },
        { SRV_DICT_TAG_KY,     "考研" },
        { SRV_DICT_TAG_GK,     "高考" },
        { SRV_DICT_TAG_ZK,     "中考" },
        { SRV_DICT_TAG_TOEFL,  "托福" },
        { SRV_DICT_TAG_IELTS,  "雅思" },
        { SRV_DICT_TAG_GRE,    "GRE" },
        { SRV_DICT_TAG_OXFORD, "牛津" },
    };

    buf[0] = '\0';
    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
        if ((e->tags & MAP[i].bit) == 0) {
            continue;
        }
        if (buf[0] != '\0') {
            strncat(buf, " ", cap - strlen(buf) - 1);
        }
        strncat(buf, MAP[i].txt, cap - strlen(buf) - 1);
    }
    for (int i = 0; i < (int)e->collins && strlen(buf) + 4 < cap; i++) {
        strncat(buf, "★", cap - strlen(buf) - 1);
    }
}

/* 详情底部的「相关短语」块：实现在下面，try_detail 填完释义后顺手刷它 */
static void detail_fill_phrases(const srv_dict_entry_t *e);

/**
 * @brief 查整词并切到详情视图（带词形还原）
 * @return false 表示词库里没有这个词（调用方接着做前缀联想）
 */
static bool try_detail(const char *word)
{
    static srv_dict_entry_t e;
    /* 先查英汉，查不到再查中英（汉字输入） */
    if (srv_dict_lookup_lemma(word, &e) != ESP_OK &&
        srv_dict_zh_lookup(word, &e) != ESP_OK) {
        return false;
    }

    str_copy_trunc(s_detail_word_txt, sizeof(s_detail_word_txt), e.word);
    lv_label_set_text(s_detail_word, e.word);
    lv_label_set_text(s_detail_ph, e.phonetic[0] ? e.phonetic : "");
    lv_label_set_text(s_detail_def, e.def[0] ? e.def : "（无释义）");

    char badge[64];
    build_badges(&e, badge, sizeof(badge));
    lv_label_set_text(s_detail_badge, badge);

    /* 词形提示：running → run（点一下跳到原形）。
     * 判断用 e.lemma 和 **e.word** 比，不能和输入词比：输入 `runnning` 时
     * 规则还原命中 `run`，此时 e.lemma 也被设成 `run`，不该再提示「→ run」。 */
    if (e.lemma[0] != '\0' && strcmp(e.lemma, e.word) != 0) {
        char hint[64];
        snprintf(hint, sizeof(hint), "→ %s 查看", e.lemma);
        str_copy_trunc(s_lemma_txt, sizeof(s_lemma_txt), e.lemma);
        lv_label_set_text(s_detail_lemma, hint);
        lv_obj_remove_flag(s_detail_lemma, LV_OBJ_FLAG_HIDDEN);
    } else {
        s_lemma_txt[0] = '\0';
        lv_obj_add_flag(s_detail_lemma, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_scroll_to_y(s_detail, 0, LV_ANIM_OFF);
    view_show(VIEW_DETAIL);
    detail_fill_phrases(&e);            /* 释义定下来了才能算短语块的位置 */
    return true;
}

/* 点「→ run 查看」：跳到原形；返回时回上一级视图 */
static void on_lemma_clicked(lv_event_t *e)
{
    (void)e;
    if (s_lemma_txt[0] == '\0') {
        return;
    }
    bool from_trans = s_detail_from_trans;
    if (try_detail(s_lemma_txt)) {
        s_detail_from_trans = from_trans;   /* 保留"从哪来" */
    }
}

static void on_detail_back(lv_event_t *e)
{
    (void)e;
    view_show(s_detail_from_trans ? VIEW_TRANS : VIEW_LIST);
    s_detail_from_trans = false;
}

/* ===================== 相关短语 =====================
 *
 * 放在释义底下，列「以当前词开头、且本身是短语」的词条：查 `look` 能得到
 * look after / look at / look for …，查 `give up` 能得到 give up on 之类。
 *
 * 只收带空格的条目 —— 单词的前缀联想在列表视图里已经有了，详情里再列一遍
 * 只是噪音；而且「相关短语」这个名字本来就指的是词组。
 */
#define REL_TITLE_H  20         /*!< 「相关短语」标题占的高度 */
#define REL_ROW_H    26
#define REL_ROW_GAP  4
#define REL_MAX      6          /*!< 最多列几条 */

/* s_rel / s_rel_skip 的状态声明在文件头的页面状态块里（销毁回调要用） */

/* 点一条短语：跳到查它（跟「→ run 查看」一样，保留“从哪来”） */
static void on_rel_clicked(lv_event_t *e)
{
    lv_obj_t *lb = (lv_obj_t *)lv_obj_get_user_data(lv_event_get_target(e));
    if (lb == NULL) {
        return;
    }
    bool from_trans = s_detail_from_trans;
    if (try_detail(lv_label_get_text(lb))) {
        s_detail_from_trans = from_trans;
    }
}

static bool rel_cb(const srv_dict_entry_t *entry, void *arg)
{
    int *n = (int *)arg;
    if (*n >= REL_MAX) {
        return false;
    }
    if (strchr(entry->word, ' ') == NULL) {
        return true;                        /* 只要短语，跳过单词 */
    }
    if (strcmp(entry->word, s_rel_skip) == 0) {
        return true;                        /* 排除自身 */
    }

    lv_obj_t *row = lv_button_create(s_rel);
    lv_obj_set_size(row, BODY_W, REL_ROW_H);
    lv_obj_set_pos(row, 0, REL_TITLE_H + *n * (REL_ROW_H + REL_ROW_GAP));
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_bg_color(row, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    ui_add_click_feedback(row);

    lv_obj_t *lb = lv_label_create(row);
    lv_obj_set_style_text_color(lb, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(lb, &font_ui_14, 0);
    lv_label_set_text(lb, entry->word);
    lv_obj_set_width(lb, BODY_W - 24);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
    lv_obj_align(lb, LV_ALIGN_LEFT_MID, 12, 0);

    /* 点击时要知道查哪个词：词标签挂在行对象的 user_data 上（同列表视图） */
    lv_obj_set_user_data(row, lb);
    lv_obj_add_event_cb(row, on_rel_clicked, LV_EVENT_CLICKED, NULL);

    (*n)++;
    return true;
}

/* 前置声明：详情底部的短语块要用它判断中文/英文 */
static bool is_zh_text(const char *s);

static void detail_fill_phrases(const srv_dict_entry_t *e)
{
    if (s_rel == NULL) {
        return;
    }

    lv_obj_clean(s_rel);                    /* 标题和行一起重建，最多 6 行，不心疼 */
    str_copy_trunc(s_rel_skip, sizeof(s_rel_skip), e->word);

    int n = 0;
    if (is_zh_text(e->word)) {
        srv_dict_zh_prefix(e->word, rel_cb, &n, REL_MAX);
    } else {
        srv_dict_prefix(e->word, rel_cb, &n, REL_MAX);
    }

    if (n == 0) {
        lv_obj_add_flag(s_rel, LV_OBJ_FLAG_HIDDEN);     /* 没有就整块收起来 */
        return;
    }

    lv_obj_t *ttl = lv_label_create(s_rel);
    lv_obj_set_style_text_color(ttl, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(ttl, &font_ui_14, 0);
    lv_obj_set_pos(ttl, 2, 0);
    lv_label_set_text(ttl, "相关短语");

    lv_obj_set_size(s_rel, BODY_W, REL_TITLE_H + n * (REL_ROW_H + REL_ROW_GAP));

    /* 释义标签的高度是自适应的（长释义会换行），先让布局算完再贴到它下面 */
    lv_obj_update_layout(s_detail);
    lv_obj_set_pos(s_rel, 0, lv_obj_get_y(s_detail_def) + lv_obj_get_height(s_detail_def) + 8);
    lv_obj_remove_flag(s_rel, LV_OBJ_FLAG_HIDDEN);
}

/* ===================== 联想 / 建议列表 ===================== */

static void on_hit_clicked(lv_event_t *e)
{
    lv_obj_t *lb = (lv_obj_t *)lv_obj_get_user_data(lv_event_get_target(e));
    if (lb == NULL) {
        return;
    }
    s_detail_from_trans = false;
    try_detail(lv_label_get_text(lb));
}

/* 一行：左词右音标（联想列表用）
 *
 * exact = true 表示这一条是输入词的整词命中（或词形还原命中）：
 * 用主色描边突出显示，但**仍然要用户自己点**才进详情。 */
static void add_word_row(const srv_dict_entry_t *e, bool exact)
{
    lv_obj_t *row = lv_button_create(s_list);
    lv_obj_set_size(row, BODY_W, ROW_H);
    lv_obj_set_pos(row, 0, s_row_y);
    s_row_y += ROW_H + ROW_GAP;
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);

    if (exact) {
        /* 深色底上 30% 的主色是一层很淡的蓝，配 1px 描边足够区分，
         * 又不会像实心主色块那样抢眼 */
        lv_obj_set_style_bg_color(row, UI_COLOR_PRIMARY, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_30, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, UI_COLOR_PRIMARY, 0);
    } else {
        lv_obj_set_style_bg_color(row, UI_COLOR_SURFACE, 0);
    }

    ui_add_click_feedback(row);
    lv_obj_add_event_cb(row, on_hit_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *wl = lv_label_create(row);
    lv_obj_set_style_text_color(wl, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(wl, &font_ui_14, 0);
    lv_label_set_text(wl, e->word);
    lv_obj_set_width(wl, 150);
    lv_label_set_long_mode(wl, LV_LABEL_LONG_DOT);
    lv_obj_align(wl, LV_ALIGN_LEFT_MID, 12, 0);

    if (e->phonetic[0] != '\0') {
        lv_obj_t *pl = lv_label_create(row);
        lv_obj_set_style_text_color(pl, UI_COLOR_TEXT_DIM, 0);
        /* 音标里的 IPA 符号不在中文 UI 字体里，必须用音标专用字体 */
        lv_obj_set_style_text_font(pl, &font_ipa_14, 0);
        lv_label_set_text(pl, e->phonetic);
        lv_obj_set_width(pl, 124);
        lv_label_set_long_mode(pl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(pl, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(pl, LV_ALIGN_RIGHT_MID, -12, 0);
    }

    /* 点击行时要知道点的是哪个词：把词标签挂在行对象的 user_data 上 */
    lv_obj_set_user_data(row, wl);
}

/* 一行文字提示（左对齐，占一行高度） */
static void add_text_row(const char *text, uint32_t color)
{
    lv_obj_t *lb = lv_label_create(s_list);
    lv_obj_set_style_text_color(lb, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(lb, &font_ui_14, 0);
    lv_obj_set_width(lb, BODY_W - 8);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
    lv_label_set_text(lb, text);
    lv_obj_set_pos(lb, 4, s_row_y + 8);
    s_row_y += 28;
}

/* 建议行：「是不是要找：apple」，点了直进 apple */
static void on_sug_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (idx < 0 || idx >= SUGGEST_MAX || s_sug_text[idx][0] == '\0') {
        return;
    }
    s_detail_from_trans = false;
    try_detail(s_sug_text[idx]);
}

static bool suggest_cb(const srv_dict_entry_t *e, void *arg)
{
    int *n = (int *)arg;
    if (*n >= SUGGEST_MAX) {
        return false;
    }
    str_copy_trunc(s_sug_text[*n], WORD_LEN, e->word);

    char line[WORD_LEN + 24];
    snprintf(line, sizeof(line), "是不是要找：%s", e->word);
    add_text_row(line, 0x1585C0);       /* UI_COLOR_PRIMARY */

    /* 把"点哪一行对应哪个词"记在最后一条提示 label 上（label 是刚建的那条） */
    lv_obj_t *lb = lv_obj_get_child(s_list, -1);
    lv_obj_add_flag(lb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(lb, (void *)(intptr_t)(*n));
    lv_obj_add_event_cb(lb, on_sug_clicked, LV_EVENT_CLICKED, NULL);

    (*n)++;
    return true;
}

/*! fill_list 的参数：已显示条数 + 要跳过的词（整词命中的那条已经在上方） */
typedef struct {
    int         count;
    const char *skip;
} fill_arg_t;

static bool fill_hit_cb(const srv_dict_entry_t *e, void *arg)
{
    fill_arg_t *a = (fill_arg_t *)arg;
    if (a->count >= LIST_MAX) {
        return false;
    }
    if (a->skip != NULL && strcmp(e->word, a->skip) == 0) {
        return true;                    /* 跳过重复的整词，继续往下收 */
    }
    add_word_row(e, false);
    a->count++;
    return true;
}

/* 列出以 prefix 开头的词（列表已清空、s_row_y 已归零）；返回新增条数 */
static int fill_list(const char *prefix, const char *skip)
{
    fill_arg_t a = { .count = 0, .skip = skip };
    srv_dict_prefix(prefix, fill_hit_cb, &a, LIST_MAX);
    if (a.count > 0) {
        lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
    }
    return a.count;
}

/* 未命中时的拼写建议：几条「是不是要找：xxx」放在列表最上面。返回条数 */
static int fill_suggest(const char *word)
{
    int n = 0;
    for (int i = 0; i < SUGGEST_MAX; i++) {
        s_sug_text[i][0] = '\0';
    }
    srv_dict_suggest(word, suggest_cb, &n, SUGGEST_MAX);
    return n;
}

/* ===================== 直译视图 ===================== */

/* 一个 token 一行：左原文 / 右中文首义；整行可点进详情 */
static void on_trans_clicked(lv_event_t *e)
{
    lv_obj_t *lb = (lv_obj_t *)lv_obj_get_user_data(lv_event_get_target(e));
    if (lb == NULL) {
        return;
    }
    s_detail_from_trans = true;
    if (!try_detail(lv_label_get_text(lb))) {
        s_detail_from_trans = false;
    }
}

static bool trans_row_cb(const char *token, const srv_dict_entry_t *e, void *arg)
{
    lv_coord_t *y = (lv_coord_t *)arg;

    lv_obj_t *row = lv_obj_create(s_trans);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, BODY_W);
    lv_obj_set_height(row, 32);
    lv_obj_set_pos(row, 0, *y);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_bg_color(row, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    ui_add_click_feedback(row);

    /* 左：原文 */
    lv_obj_t *tl = lv_label_create(row);
    lv_obj_set_style_text_color(tl, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(tl, &font_ui_14, 0);
    lv_label_set_text(tl, token);
    lv_obj_set_width(tl, 96);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
    lv_obj_align(tl, LV_ALIGN_LEFT_MID, 8, 0);

    /* 右：中文（只取最前面那几个义项，多的用 … 收掉）
     * 注意不能写 '；'（UTF-8 多字节，编译器会报 multi-character constant），
     * 用 strstr 找第二个分号即可 */
    char meaning[96];
    if (e != NULL) {
        const char *first  = strstr(e->def, "；");
        const char *second = (first != NULL) ? strstr(first + 3, "；") : NULL;
        size_t      n      = (second != NULL) ? (size_t)(second - e->def) : strlen(e->def);
        if (n > sizeof(meaning) - 4) {
            n = sizeof(meaning) - 4;
        }
        memcpy(meaning, e->def, n);
        meaning[n] = '\0';
        if (second != NULL) {
            strncat(meaning, "…", sizeof(meaning) - strlen(meaning) - 1);
        }
    } else {
        str_copy_trunc(meaning, sizeof(meaning), "—");
    }

    lv_obj_t *ml = lv_label_create(row);
    lv_obj_set_style_text_font(ml, &font_ui_14, 0);
    lv_obj_set_style_text_color(ml, (e != NULL) ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_width(ml, BODY_W - 120);
    lv_label_set_long_mode(ml, LV_LABEL_LONG_WRAP);
    lv_label_set_text(ml, meaning);
    lv_obj_align(ml, LV_ALIGN_LEFT_MID, 110, 0);

    /* wrap 之后量一下实际高度，把行撑到刚好放下（免得长释义被裁掉） */
    lv_obj_update_layout(row);
    lv_coord_t h = lv_obj_get_height(ml) + 10;
    if (h < 32) {
        h = 32;
    }
    lv_obj_set_height(row, h);
    *y += h + 4;

    if (e != NULL) {
        lv_obj_add_event_cb(row, on_trans_clicked, LV_EVENT_CLICKED, NULL);
        lv_obj_set_user_data(row, tl);
    }
    return true;
}

/* 直译整句：清空后逐词建行 */
static void fill_translate(const char *text)
{
    lv_obj_clean(s_trans);

    lv_coord_t y = 0;
    srv_dict_translate(text, trans_row_cb, &y);
    lv_obj_scroll_to_y(s_trans, 0, LV_ANIM_OFF);
}

static void trans_hint(void)
{
    lv_obj_clean(s_trans);

    lv_obj_t *lb = lv_label_create(s_trans);
    lv_obj_set_style_text_color(lb, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lb, &font_ui_14, 0);
    lv_label_set_text(lb, "点上方搜索框输入英文句子\n按词给中文释义，点某一行看完整词条");
    lv_obj_set_width(lb, BODY_W - 16);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lb, LV_ALIGN_CENTER, 0, 0);
}

/* ===================== 模式与查询入口 ===================== */

static void apply_tab_style(void)
{
    lv_obj_t *on  = (s_mode == 0) ? s_tab_word : s_tab_trans;
    lv_obj_t *off = (s_mode == 0) ? s_tab_trans : s_tab_word;

    lv_obj_set_style_bg_color(on, UI_COLOR_PRIMARY, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(on, 0), lv_color_white(), 0);
    lv_obj_set_style_bg_color(off, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(off, 0), UI_COLOR_TEXT_DIM, 0);
}

static void set_placeholder(void)
{
    lv_label_set_text(s_search_lb,
                      (s_mode == 0) ? "输入单词，如 apple" : "输入句子，如 hello world");
    lv_obj_set_style_text_color(s_search_lb, UI_COLOR_TEXT_DIM, 0);
}

/* 查词模式的结果视图：**任何时候都先铺列表**，点某一行才进详情。
 *
 * 整词命中（含词形还原）也放列表里、置顶并用主色描边标出来 —— 15 万词库下
 * 常见词很容易撞上整词，自动跳走会让人没法继续挑（比如想查 `run` 的派生词，
 * 刚打完 `run` 就被带进详情，还得退回来）。
 * 没命中时上面是「是不是要找 xxx」，下面是以输入开头的词。 */
/* 查词结果：中文走 CC-CEDICT，英文走 ECDICT；英文查不到时再按拼音试一次 */
static bool is_zh_text(const char *s)
{
    for (; *s != '\0'; s++) {
        if ((unsigned char)*s >= 0x80) {
            return true;
        }
    }
    return false;
}

static int fill_zh_list(const char *prefix, const char *skip)
{
    fill_arg_t a = { .count = 0, .skip = skip };
    srv_dict_zh_prefix(prefix, fill_hit_cb, &a, LIST_MAX);
    if (a.count > 0) lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
    return a.count;
}

static int fill_zh_pinyin(const char *py)
{
    fill_arg_t a = { .count = 0, .skip = NULL };
    srv_dict_zh_pinyin(py, fill_hit_cb, &a, LIST_MAX);
    if (a.count > 0) lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
    return a.count;
}

static void show_zh_results(void)
{
    lv_obj_clean(s_list);
    s_row_y = 0;

    static srv_dict_entry_t e;
    const bool exact = (srv_dict_zh_lookup(s_word, &e) == ESP_OK);
    if (exact) {
        add_word_row(&e, true);
    }
    int n = fill_zh_list(s_word, exact ? e.word : NULL);

    if (!exact && n == 0) {
        list_hint("未收录该词");
    }
    view_show(VIEW_LIST);
}

static void show_word_results(void)
{
    /* 汉字输入：直接查中英词典（CC-CEDICT） */
    if (is_zh_text(s_word)) {
        if (srv_dict_zh_init() == ESP_OK) {
            show_zh_results();
        } else {
            lv_obj_clean(s_list);
            s_row_y = 0;
            list_hint("中文词典不可用：SD 卡 /dict 下要有 zhdict.dat / zhdict.idx");
            view_show(VIEW_LIST);
        }
        return;
    }

    lv_obj_clean(s_list);
    s_row_y = 0;

    static srv_dict_entry_t e;
    const bool exact = (srv_dict_lookup_lemma(s_word, &e) == ESP_OK);
    if (exact) {
        add_word_row(&e, true);
    }

    int n = 0;
    if (!exact) {
        n += fill_suggest(s_word);
    }
    n += fill_list(s_word, exact ? e.word : NULL);

    /* 英文词库没有，再按无声调拼音试一次（pingguo / chuan tong） */
    if (!exact && n == 0 && srv_dict_zh_init() == ESP_OK) {
        n += fill_zh_pinyin(s_word);
    }

    if (!exact && n == 0) {
        list_hint("未收录该词");
    }
    view_show(VIEW_LIST);
}

/* 切模式：只翻隐藏 + 换配色，不重建页面（避免闪烁） */
static void switch_mode(int mode)
{
    if (mode == s_mode) {
        return;
    }
    s_mode = mode;
    apply_tab_style();
    set_placeholder();

    if (mode == 1) {
        if (s_sent[0] != '\0') {
            fill_translate(s_sent);
        } else {
            trans_hint();
        }
        view_show(VIEW_TRANS);
    } else {
        if (s_word[0] != '\0') {
            lv_label_set_text(s_search_lb, s_word);
            lv_obj_set_style_text_color(s_search_lb, UI_COLOR_TEXT, 0);
            show_word_results();
        } else {
            list_hint("点上方搜索框输入单词");
            view_show(VIEW_LIST);
        }
    }
    s_detail_from_trans = false;
}

static void on_tab_word(lv_event_t *e)
{
    (void)e;
    switch_mode(0);
}

static void on_tab_trans(lv_event_t *e)
{
    (void)e;
    switch_mode(1);
}

/* 键盘「确定」回来：text 是输入框内容 */
static void on_text_done(const char *text, void *arg)
{
    (void)arg;

    if (s_owner == NULL || text == NULL || text[0] == '\0') {
        return;
    }

    lv_label_set_text(s_search_lb, text);
    lv_obj_set_style_text_color(s_search_lb, UI_COLOR_TEXT, 0);
    s_detail_from_trans = false;

    /* 词库不可用：分清是没插卡还是没词库文件，别让用户以为是"没收录" */
    if (srv_dict_init() != ESP_OK) {
        if (!srv_sd_is_mounted()) {
            list_hint("SD 卡未挂载");
        } else {
            list_hint("词库不可用：SD 卡的 /dict 下要有 dict.dat 与 dict.idx\n"
                      "（用 tools/build_dict.py 生成）");
        }
        view_show(VIEW_LIST);
        return;
    }

    if (s_mode == 1) {
        str_copy_trunc(s_sent, sizeof(s_sent), text);
        fill_translate(s_sent);
        view_show(VIEW_TRANS);
        return;
    }

    str_copy_trunc(s_word, sizeof(s_word), text);
    show_word_results();
}

static void on_search_clicked(lv_event_t *e)
{
    (void)e;
    if (s_owner == NULL) {
        return;
    }
    if (s_mode == 1) {
        /* 翻译要的是英文句子，保持英文键盘 */
        ui_text_input_show_init("翻译（英文句子）", SENT_LEN, s_sent, on_text_done, NULL);
    } else {
        /* 查词：预填上一次的词，方便加字母/改错；中文用拼音打 */
        ui_text_input_show_init("查词", WORD_LEN, s_word, on_text_done, NULL);
    }
}

/* ===================== 页面构建 ===================== */

/* 标题栏右侧的模式按钮 */
static lv_obj_t *make_tab(lv_obj_t *root, lv_coord_t x, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(root);
    lv_obj_set_size(btn, TAB_W, TAB_H);
    lv_obj_set_pos(btn, x, 4);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    ui_add_click_feedback(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lb = lv_label_create(btn);
    lv_obj_set_style_text_font(lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(lb, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(lb, text);
    lv_obj_center(lb);
    return btn;
}

static void on_entry_clicked(lv_event_t *e)
{
    app_page_id_t page = (app_page_id_t)(intptr_t)lv_event_get_user_data(e);
    app_navigator_switch(page, true);
}

void ui_words_create(lv_obj_t *root)
{
    s_mode = 0;
    s_word[0] = '\0';
    s_sent[0] = '\0';
    s_lemma_txt[0] = '\0';
    s_detail_from_trans = false;

    /* 右侧留给两个模式按钮；旋转键放到返回键右侧，避免挤占居中的标题。 */
    ui_titlebar_create_plain_rot_left(root, "单词", app_navigator_back_cb, NULL);
    s_tab_word  = make_tab(root, TAB_WORD_X,  "查词", on_tab_word);
    s_tab_trans = make_tab(root, TAB_TRANS_X, "译",   on_tab_trans);

    /* 搜索框右侧两个入口：变形查询 / 单词学习 */
    static const struct {
        const char   *text;
        app_page_id_t page;
    } entries[] = {
        {"变形", PAGE_WORD_FORMS},
        {"学习", PAGE_WORD_STUDY},
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        const lv_coord_t entry_x = (i == 0) ? ENTRY_FORM_X : ENTRY_STUDY_X;
        lv_obj_t *b = lv_button_create(root);
        lv_obj_set_size(b, ENTRY_W, ENTRY_H);
        lv_obj_set_pos(b, entry_x, SEARCH_Y);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_bg_color(b, UI_COLOR_SURFACE, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        ui_add_click_feedback(b);
        lv_obj_add_event_cb(b, on_entry_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)entries[i].page);

        lv_obj_t *bl = lv_label_create(b);
        lv_obj_set_style_text_font(bl, &font_ui_14, 0);
        lv_obj_set_style_text_color(bl, UI_COLOR_TEXT, 0);
        lv_label_set_text(bl, entries[i].text);
        lv_obj_center(bl);
    }

    /* ===== 搜索框（点它弹屏幕键盘） ===== */
    lv_obj_t *box = lv_button_create(root);
    lv_obj_set_size(box, SEARCH_W, SEARCH_H);
    lv_obj_set_pos(box, SEARCH_X, SEARCH_Y);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_set_style_bg_color(box, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_shadow_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    ui_add_click_feedback(box);
    lv_obj_add_event_cb(box, on_search_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *sic = lv_image_create(box);
    lv_image_set_src(sic, &ic_search);
    lv_obj_set_style_image_recolor(sic, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_radius(sic, 0, 0);
    lv_image_set_scale(sic, 20 * 256 / 32);
    lv_obj_align(sic, LV_ALIGN_LEFT_MID, 10, 0);

    s_search_lb = lv_label_create(box);
    lv_obj_set_style_text_color(s_search_lb, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_search_lb, &font_ui_14, 0);
    lv_obj_set_width(s_search_lb, SEARCH_W - 48);
    lv_label_set_long_mode(s_search_lb, LV_LABEL_LONG_DOT);
    lv_obj_align(s_search_lb, LV_ALIGN_LEFT_MID, 38, 0);

    /* ===== 详情视图里的「返回」 ===== */
    s_back_btn = lv_button_create(root);
    lv_obj_set_size(s_back_btn, 26, 22);
    lv_obj_set_pos(s_back_btn, BODY_X, BODY_Y);
    lv_obj_set_style_radius(s_back_btn, 6, 0);
    lv_obj_set_style_bg_color(s_back_btn, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_shadow_width(s_back_btn, 0, 0);
    ui_add_click_feedback(s_back_btn);
    lv_obj_add_event_cb(s_back_btn, on_detail_back, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *ic = lv_image_create(s_back_btn);
        lv_image_set_src(ic, &ic_arrow_left);
        lv_obj_set_style_image_recolor(ic, UI_COLOR_TEXT, 0);
        lv_obj_set_style_radius(ic, 0, 0);
        lv_obj_center(ic);
    }

    /* ===== 联想列表 ===== */
    s_list = lv_obj_create(root);
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_size(s_list, BODY_W, BODY_H);
    lv_obj_set_pos(s_list, BODY_X, BODY_Y);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_radius(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_style_width(s_list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_list, UI_COLOR_TEXT_DIM, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_list, 2, LV_PART_SCROLLBAR);

    /* ===== 释义详情 ===== */
    s_detail = lv_obj_create(root);
    lv_obj_remove_flag(s_detail, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_size(s_detail, BODY_W, BODY_H);
    lv_obj_set_pos(s_detail, BODY_X, BODY_Y);
    lv_obj_set_style_bg_opa(s_detail, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_detail, 0, 0);
    lv_obj_set_style_radius(s_detail, 0, 0);
    lv_obj_set_style_pad_all(s_detail, 0, 0);
    lv_obj_set_scroll_dir(s_detail, LV_DIR_VER);
    lv_obj_set_style_width(s_detail, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_detail, UI_COLOR_TEXT_DIM, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_detail, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_detail, 2, LV_PART_SCROLLBAR);

    /* 第一行左边给「返回」按钮留出位置 */
    s_detail_word = lv_label_create(s_detail);
    lv_obj_set_style_text_color(s_detail_word, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_detail_word, &font_ui_14, 0);
    lv_obj_set_width(s_detail_word, BODY_W - 44);
    lv_label_set_long_mode(s_detail_word, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_detail_word, 34, 3);

    /* 徽章：单独占一行，免得跟词/音标挤在一起换行（LONG_DOT 必须显式给高度） */
    s_detail_badge = lv_label_create(s_detail);
    lv_obj_set_style_text_color(s_detail_badge, UI_COLOR_PRIMARY, 0);
    lv_obj_set_style_text_font(s_detail_badge, &font_ui_14, 0);
    lv_obj_set_pos(s_detail_badge, 34, 42);
    lv_obj_set_width(s_detail_badge, BODY_W - 34);
    lv_obj_set_height(s_detail_badge, 18);
    lv_label_set_long_mode(s_detail_badge, LV_LABEL_LONG_DOT);

    s_detail_ph = lv_label_create(s_detail);
    lv_obj_set_style_text_color(s_detail_ph, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_detail_ph, &font_ipa_14, 0);
    lv_obj_set_width(s_detail_ph, BODY_W / 2 - 34);
    lv_label_set_long_mode(s_detail_ph, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_detail_ph, 34, 23);

    /* 词形提示（可点）：→ run 查看 */
    s_detail_lemma = lv_label_create(s_detail);
    lv_obj_set_style_text_color(s_detail_lemma, UI_COLOR_PRIMARY, 0);
    lv_obj_set_style_text_font(s_detail_lemma, &font_ui_14, 0);
    lv_obj_set_width(s_detail_lemma, BODY_W / 2);
    lv_obj_set_style_text_align(s_detail_lemma, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_detail_lemma, BODY_W / 2, 23);
    lv_obj_add_flag(s_detail_lemma, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_detail_lemma, on_lemma_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_detail_lemma, LV_OBJ_FLAG_HIDDEN);

    s_detail_def = lv_label_create(s_detail);
    lv_obj_set_style_text_color(s_detail_def, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_detail_def, &font_ui_14, 0);
    lv_obj_set_width(s_detail_def, BODY_W);
    lv_label_set_long_mode(s_detail_def, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_detail_def, 0, 64);

    /* 相关短语：内容随词变，位置也要等释义高度算出来才知道，所以先建好藏着，
     * 每次进详情由 detail_fill_phrases() 重建内容并摆到释义下面 */
    s_rel = lv_obj_create(s_detail);
    lv_obj_remove_flag(s_rel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_rel, BODY_W, REL_TITLE_H);
    lv_obj_set_style_bg_opa(s_rel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_rel, 0, 0);
    lv_obj_set_style_radius(s_rel, 0, 0);
    lv_obj_set_style_pad_all(s_rel, 0, 0);
    lv_obj_add_flag(s_rel, LV_OBJ_FLAG_HIDDEN);

    /* ===== 直译视图 ===== */
    s_trans = lv_obj_create(root);
    lv_obj_remove_flag(s_trans, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_size(s_trans, BODY_W, BODY_H);
    lv_obj_set_pos(s_trans, BODY_X, BODY_Y);
    lv_obj_set_style_bg_opa(s_trans, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_trans, 0, 0);
    lv_obj_set_style_radius(s_trans, 0, 0);
    lv_obj_set_style_pad_all(s_trans, 0, 0);
    lv_obj_set_scroll_dir(s_trans, LV_DIR_VER);
    lv_obj_set_style_width(s_trans, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_trans, UI_COLOR_TEXT_DIM, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_trans, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_trans, 2, LV_PART_SCROLLBAR);

    s_owner = root;
    lv_obj_add_event_cb(root, words_del_cb, LV_EVENT_DELETE, NULL);

    /* 「返回」按钮建在 root 上、和详情容器同位置，而详情容器是后建的，
     * 会把它整个盖住 —— 不提到最前面，详情里的返回键就点不动。 */
    lv_obj_move_foreground(s_back_btn);

    /* 初始态：查词模式 + 列表视图；词库索引是同步读的（约 30ms），
     * 没就绪就顺手报一句缺什么 */
    apply_tab_style();
    set_placeholder();
    if (srv_dict_init() == ESP_OK) {
        list_hint("点上方搜索框输入单词");
    } else if (!srv_sd_is_mounted()) {
        list_hint("SD 卡未挂载");
    } else {
        list_hint("词库不可用：SD 卡的 /dict 下要有 dict.dat 与 dict.idx");
    }
    view_show(VIEW_LIST);
}