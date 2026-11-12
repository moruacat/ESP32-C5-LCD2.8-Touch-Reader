#include "ui_reader.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "ui_dialog.h"
#include "app_navigator.h"
#include "srv_reader.h"
#include "srv_library.h"
#include "srv_config.h"

#include "esp_log.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ui_reader";

/*! 取章名用的缓冲。滚轮一行也就显示十来个字，这么长够用了 */
#define CHAP_TITLE_MAX 96

#define RD_MARGIN     8
#define RD_BOTTOM_H   18                    /*!< 底部页码栏高度 */

/* 正文可用区域 */
#define PAGE_TEXT_W   (UI_SCREEN_W - RD_MARGIN * 2)
#define PAGE_TEXT_H   (UI_SCREEN_H - UI_TITLEBAR_H - RD_BOTTOM_H - 8)

#define SCAN_CHARS    600                   /*!< 每次多读一点，保证够排满一页 */
#define SCAN_BYTES    (SCAN_CHARS * 4 + 8)  /*!< 最坏每字符 4 字节 */

#define PAGE_MAX      1500                  /*!< 页起点表上限 */

/* 页起点表：s_page_start[i] = 第 i 页第一个字的字符偏移。
 * 往后翻时逐页推导并缓存，往回翻直接查表。 */
static int32_t s_page_start[PAGE_MAX];
static int     s_page_cnt = 0;
static int     s_page_cur = 0;

static char    s_scan[SCAN_BYTES];          /* 从 reader 读出的原文 */
static char    s_page[SCAN_BYTES + 1024];   /* 排版好的一页文本 */

static lv_obj_t *s_text_lb  = NULL;         /* 正文 */
static lv_obj_t *s_info_lb  = NULL;         /* 底部页码 */
static lv_obj_t *s_measure  = NULL;         /* 隐藏的测量用 label */
static lv_obj_t *s_titlebar = NULL;         /* 标题栏（书名要等打开完才知道，得留着改字） */

/* 打开 / 排版期间的等待层与定时器 */
static lv_obj_t   *s_wait_layer   = NULL;
static lv_obj_t   *s_wait_note    = NULL;
static lv_obj_t   *s_wait_bar     = NULL;   /* 进度条 */
static lv_obj_t   *s_wait_pct     = NULL;   /* 条上那个百分比 */
static lv_timer_t *s_open_poll    = NULL;   /* 轮询后台打开是否完成 */
static lv_timer_t *s_build_timer  = NULL;   /* 分片建页表 */
static int32_t     s_build_target = 0;      /* 页表要推到哪个字符偏移 */

/* 进度条把「打开」和「排版」两段合成一条：打开占前 40%，排版占后 60%。
 * 分两条的话会走满一次又从头开始，看着像卡住重来。 */
#define WAIT_OPEN_PCT   40

/* ===================== 排版参数（来自「设置 → 阅读」） ===================== */

/*! 段距折算成段间空行后，一页文本最多膨胀到这个长度 */
#define PAGE_EXPANDED_MAX  (SCAN_BYTES * 2 + 1024)

static int  s_line_space   = 6;     /*!< 行距（额外） */
static int  s_letter_space = 0;     /*!< 字距 */
static int  s_para_blanks  = 0;     /*!< 段间空行数 */

/* 渲染/测量共用的膨胀缓冲：两者不同时用，省一份 RAM */
static char s_expand_buf[PAGE_EXPANDED_MAX];

/**
 * @brief 读阅读设置
 *
 * 字号暂时没有对应字体（项目里只有 14px 汉字字体），所以没接进来；
 * read_font_size 配置保留着，等生成了多档字体再补。
 */
static void read_style_load(void)
{
    s_line_space   = (int)srv_config_get_int(CFG_READ_LINE_SP, 6);
    s_letter_space = (int)srv_config_get_int(CFG_READ_LETTER_SP, 0);

    /* 段距：LVGL 的 label 没有「段间距」，行高由字体定死，做不到像素级精确，
     * 只能按整行近似 —— 段间多插几个空行。设置页里这一项是 10 一格，
     * 这里 10 → 1 个空行，档位正好一一对应。 */
    int ps = (int)srv_config_get_int(CFG_READ_PARA_SP, 0);
    s_para_blanks = ps / 10;
    if (s_para_blanks < 0) {
        s_para_blanks = 0;
    }
}

/* 正文和测量 label 的排版参数必须完全一致，否则量出来的高度不作数 */
static void text_style_apply(lv_obj_t *lb)
{
    lv_obj_set_style_text_font(lb, &font_ui_14, 0);
    lv_obj_set_style_text_line_space(lb, s_line_space, 0);
    lv_obj_set_style_text_letter_space(lb, s_letter_space, 0);
}

/**
 * @brief 把段间的换行扩展成「换行 + N 个空行」
 *
 * 膨胀只发生在渲染/测量用的临时缓冲里，字符数仍然按原文算，
 * 所以分页边界和阅读进度都不受影响。
 */
static void expand_paragraphs(const char *src, char *dst, size_t cap)
{
    size_t p = 0;

    if (cap == 0) {
        return;
    }

    for (const char *s = src; *s != '\0'; s++) {
        if (*s == '\n') {
            for (int i = 0; i <= s_para_blanks; i++) {
                if (p + 1 >= cap) {
                    break;
                }
                dst[p++] = '\n';
            }
        } else {
            if (p + 1 >= cap) {
                break;
            }
            dst[p++] = *s;
        }
    }
    dst[p] = '\0';
}

/* ===================== UTF-8 小工具 ===================== */

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

static int utf8_count(const char *s)
{
    int len = (int)strlen(s);
    int pos = 0;
    int n = 0;

    while (pos < len) {
        pos += utf8_seq_len((uint8_t)s[pos]);
        n++;
    }
    return n;
}

/**
 * @brief 把 src 的前 n 个字符拷进 dst，不切断多字节字符
 * @return 实际写入的字节数
 */
static int utf8_copy_n(const char *src, int n, char *dst, size_t dst_len)
{
    int len = (int)strlen(src);
    int pos = 0;
    int cnt = 0;
    size_t o = 0;

    while (pos < len && cnt < n) {
        int cl = utf8_seq_len((uint8_t)src[pos]);
        if (o + (size_t)cl + 1 > dst_len) {
            break;
        }
        memcpy(dst + o, src + pos, (size_t)cl);
        o += (size_t)cl;
        pos += cl;
        cnt++;
    }

    dst[o] = '\0';
    return (int)o;
}

/* ===================== 分页 ===================== */

/**
 * @brief 量一段文本在正文宽度下渲染出来有多高
 *
 * 用真的 label 去排版，而不是自己按字宽累加。理由是：自己算的宽度和
 * LVGL 实际渲染总有细微出入（字距、断行规则、中英混排的断点），
 * 一旦估小了，label 就会自己再折一次行、总高超出可见区，
 * 多出来的行被裁掉 —— 表现就是「中间缺了一大段」。
 */
static lv_coord_t measure_height(const char *text)
{
    /* 量之前要跟渲染时做同样的段落膨胀，否则段距一调大，量出来的高度就偏小、
     * 分页就会多塞几行，超出可见区被裁掉 */
    if (s_para_blanks > 0) {
        expand_paragraphs(text, s_expand_buf, sizeof(s_expand_buf));
        lv_label_set_text(s_measure, s_expand_buf);
    } else {
        lv_label_set_text(s_measure, text);
    }
    lv_obj_update_layout(s_measure);
    return lv_obj_get_height(s_measure);
}

/**
 * @brief 二分找出「渲染高度不超过一屏」的最大字符数
 *
 * 排版结果随字数单调不减，所以能二分。一次约 10 轮，每轮排 600 字
 * 在 LVGL 里是亚毫秒级，翻页感觉不出来。
 */
static int fit_chars(const char *text, int total, char *out, size_t out_len)
{
    int lo = 1;
    int hi = total;
    int best = 0;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;

        if (utf8_copy_n(text, mid, out, out_len) <= 0) {
            hi = mid - 1;
            continue;
        }

        if (measure_height(out) <= PAGE_TEXT_H) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    if (best > 0) {
        utf8_copy_n(text, best, out, out_len);
    }
    return best;
}

/**
 * @brief 从 text 里切出正好放满一页的一段，写进 out
 * @return 用掉的字符数
 */
static int layout_page(const char *text, char *out, size_t out_len)
{
    int total = utf8_count(text);
    if (total <= 0) {
        return 0;
    }
    return fit_chars(text, total, out, out_len);
}

/* ===================== 页表 ===================== */

/**
 * @brief 从页表末尾再往后推一页，返回是否成功
 *
 * 页表只有一份，同步重建和分片重建都靠它推进，免得两处逻辑走偏。
 */
static bool build_next_page(void)
{
    if (s_page_cnt >= PAGE_MAX) {
        return false;
    }

    int prev = s_page_cnt - 1;
    int got = 0;

    if (srv_reader_read(s_page_start[prev], SCAN_CHARS, s_scan, sizeof(s_scan), &got) != ESP_OK ||
        got <= 0) {
        return false;                   /* 到头了 */
    }

    int used = layout_page(s_scan, s_page, sizeof(s_page));
    if (used <= 0) {
        return false;
    }

    /* 新页起点落在章末之后 = 本章没有下一页了。
     * 不判这一下的话会往页表里记一个指向章末的「假页」，翻到它时
     * 读不出字、show_page 直接返回，看着就是「翻页没反应」。 */
    int next_start = s_page_start[prev] + used;
    if (next_start >= srv_reader_char_count()) {
        return false;
    }

    s_page_start[s_page_cnt] = next_start;
    s_page_cnt++;
    return true;
}

/* 自动翻页：每次换页都要把本页的计时归零（实现见「自动翻页」一节） */
static void auto_reset(void);

/**
 * @brief 渲染第 idx 页；页起点没算到这一页就先往后推导
 */
static void show_page(int idx)
{
    if (idx < 0 || idx >= PAGE_MAX || s_text_lb == NULL || s_info_lb == NULL) {
        return;
    }

    while (s_page_cnt <= idx) {
        if (!build_next_page()) {
            return;                     /* 到头了 */
        }
    }

    if (idx >= s_page_cnt) {
        return;
    }

    int start = s_page_start[idx];
    int got = 0;

    if (srv_reader_read(start, SCAN_CHARS, s_scan, sizeof(s_scan), &got) != ESP_OK || got <= 0) {
        return;
    }

    int used = layout_page(s_scan, s_page, sizeof(s_page));
    if (used <= 0) {
        return;
    }

    /* 顺手把下一页起点记下，下次翻页就不用再排一遍了。
     * 只在后面真的还有字时才记：否则会留下一个指向章末的「假页」，
     * 翻到它读不出内容、看着就是翻页没反应（跨章判断也会被它挡住）。 */
    if (idx + 1 == s_page_cnt && s_page_cnt < PAGE_MAX &&
        (start + used) < srv_reader_char_count()) {
        s_page_start[s_page_cnt] = start + used;
        s_page_cnt++;
    }

    s_page_cur = idx;
    if (s_para_blanks > 0) {
        expand_paragraphs(s_page, s_expand_buf, sizeof(s_expand_buf));
        lv_label_set_text(s_text_lb, s_expand_buf);
    } else {
        lv_label_set_text(s_text_lb, s_page);
    }

    int total = srv_reader_char_count();
    int pct = (total > 0) ? (start * 100 / total) : 0;
    char info[40];
    snprintf(info, sizeof(info), "第 %d 页 · %d%%", idx + 1, pct);
    lv_label_set_text(s_info_lb, info);

    srv_reader_seek(start);
    srv_reader_save_progress();

    /* 换页了，自动翻页的进度线从顶部重新扫 */
    auto_reset();
}

/* ===================== 交互 ===================== */

static uint32_t s_menu_tick;            /*!< 上次长按弹菜单的时刻 */

/**
 * @brief 判断这次点击是不是「长按弹菜单之后补发的那一下」
 *
 * LVGL 在释放时是无条件发 CLICKED 的（哪怕已经发过 LONG_PRESSED），长按弹完
 * 菜单一松手就会立刻翻一页。用时刻比对而不是布尔标志：长按后把手指滑出点击带
 * 再松开就没有 CLICKED，标志会一直留着，把下一次正常点击吃掉。
 */
static bool swallow_click_after_long(void)
{
    if (s_menu_tick != 0 && lv_tick_elaps(s_menu_tick) < 800) {
        s_menu_tick = 0;
        return true;
    }
    s_menu_tick = 0;
    return false;
}

/* ===================== 翻页（含跨章） ===================== */

/* 章名刷新在文件后半段实现（要用到标题栏对象），这里先声明 */
static void reader_update_title(void);

/**
 * @brief 切到指定章并显示
 * @param to_last true = 停在该章最后一页（往前翻进来的），false = 从第一页开始
 * @return false 表示这一章打不开，页面保持原样
 */
static bool chapter_jump(int idx, bool to_last)
{
    if (srv_reader_goto_chapter(idx) != ESP_OK) {
        ESP_LOGW(TAG, "切换章节失败：第 %d 章", idx + 1);
        return false;
    }

    reader_update_title();              /* 标题栏的章名要跟着换 */

    /* 页表是相对「当前这一章」的，换章后必须整个重来 */
    s_page_start[0] = 0;
    s_page_cnt = 1;
    s_page_cur = 0;

    if (to_last) {
        /* 停在上一章的最后一页。正文已经在内存里（goto_chapter 读过），
         * 这里只是排版，不再碰 SD。 */
        while (s_page_cnt < PAGE_MAX && build_next_page()) {
        }
        show_page(s_page_cnt - 1);
    } else {
        show_page(0);
    }
    return true;
}

/**
 * @brief 下一页：页内翻 → 排新页 → 下一章开头 → 全书完
 */
static void turn_next(void)
{
    if (s_page_cur + 1 < s_page_cnt) {
        show_page(s_page_cur + 1);
        return;
    }

    if (build_next_page()) {            /* 本章还有内容，只是还没排出来 */
        show_page(s_page_cur + 1);
        return;
    }

    /* 排不出下一页有两种原因：本章到头了，或者页表满了。
     * 页表满时内容其实还在后面，不能当成章末去跳下一章。 */
    if (s_page_cnt >= PAGE_MAX) {
        ESP_LOGW(TAG, "页表已满（%d 页），不再往后翻", PAGE_MAX);
        return;
    }

    int next = srv_reader_chapter_index() + 1;
    if (next < srv_reader_chapter_count()) {
        chapter_jump(next, false);
        return;
    }

    /* 没有下一章：TXT（整本算一章）到这里也是这个提示 */
    ui_alert_show("全书完", UI_COLOR_PRIMARY, "已经读到最后一页", "知道了",
                  NULL, NULL, 2000);
}

/**
 * @brief 上一页：页内翻 → 上一章最后一页 → 无动作（已经是全书第一页）
 */
static void turn_prev(void)
{
    if (s_page_cur > 0) {
        show_page(s_page_cur - 1);
        return;
    }

    int prev = srv_reader_chapter_index() - 1;
    if (prev >= 0) {
        chapter_jump(prev, true);
    }
    /* 没有上一章：什么都不做 */
}

/* ===================== 自动翻页 =====================
 *
 * 开启后一条细横线从正文顶部匀速扫到底部，扫到底就翻下一页。
 * 用线的位置直接表示「这页还剩多少时间」，比角落里的小图标直观。
 */

#define AUTO_LINE_H     2
#define AUTO_TICK_MS    100
#define AUTO_LINE_TOP   (UI_TITLEBAR_H + 4)
#define AUTO_LINE_BOT   (UI_SCREEN_H - RD_BOTTOM_H - 4)
#define AUTO_SEC_DEF    15
#define AUTO_SEC_MIN    5
#define AUTO_SEC_MAX    600

static lv_timer_t *s_auto_timer = NULL;
static lv_obj_t   *s_auto_line  = NULL;
static bool        s_auto_on    = false;
static bool        s_auto_hold  = false;   /*!< 菜单打开期间临时挂起，菜单关闭后可恢复 */
static int         s_auto_sec   = AUTO_SEC_DEF;
static uint32_t    s_auto_begin = 0;

static int auto_sec_load(void)
{
    int sec = (int)srv_config_get_int(CFG_READ_AUTO_SEC, AUTO_SEC_DEF);
    if (sec < AUTO_SEC_MIN) {
        sec = AUTO_SEC_MIN;
    }
    if (sec > AUTO_SEC_MAX) {
        sec = AUTO_SEC_MAX;
    }
    return sec;
}

/* 本页计时归零：线回到顶部；没开自动翻页时保持隐藏 */
static void auto_reset(void)
{
    s_auto_begin = lv_tick_get();
    if (s_auto_line == NULL) {
        return;
    }
    lv_obj_set_y(s_auto_line, AUTO_LINE_TOP);
    if (s_auto_on) {
        lv_obj_remove_flag(s_auto_line, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_auto_line, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 后面还有没有页可翻（页表内 / 本章未排完 / 还有下一章） */
static bool auto_has_next(void)
{
    if (s_page_cur + 1 < s_page_cnt) {
        return true;
    }
    if (s_page_cnt < PAGE_MAX && build_next_page()) {
        return true;
    }
    if (s_page_cnt >= PAGE_MAX) {
        return false;
    }
    return (srv_reader_chapter_index() + 1 < srv_reader_chapter_count());
}

static void auto_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_auto_on || s_auto_line == NULL) {
        return;
    }

    uint32_t total = (uint32_t)s_auto_sec * 1000u;
    uint32_t spent = lv_tick_elaps(s_auto_begin);

    if (spent >= total) {
        if (!auto_has_next()) {
            /* 到书尾就安静停下 —— 不能继续调 turn_next，
             * 那样每 100ms 会弹一次「全书完」 */
            s_auto_on = false;
            lv_obj_add_flag(s_auto_line, LV_OBJ_FLAG_HIDDEN);
            ESP_LOGI(TAG, "自动翻页到书尾，已停止");
            return;
        }
        turn_next();                /* show_page 里会把计时重置 */
        return;
    }

    lv_coord_t span = AUTO_LINE_BOT - AUTO_LINE_TOP;
    lv_coord_t y = AUTO_LINE_TOP + (lv_coord_t)((int64_t)spent * span / total);
    lv_obj_set_y(s_auto_line, y);
}

/* 开 / 关自动翻页；开启时顺带创建定时器（user_data 存 root，销毁时好判断归属） */
static void auto_set(bool on)
{
    if (s_auto_line == NULL) {
        return;
    }
    s_auto_on   = on;
    s_auto_hold = false;

    if (on && s_auto_timer == NULL) {
        lv_obj_t *root = lv_obj_get_parent(s_auto_line);
        s_auto_timer = lv_timer_create(auto_tick_cb, AUTO_TICK_MS, root);
    }

    auto_reset();
    ESP_LOGI(TAG, "自动翻页%s（间隔 %d 秒）", on ? "开启" : "关闭", s_auto_sec);
}

/* 弹菜单时先挂起，免得菜单还开着页面自己翻走 */
static void auto_suspend(void)
{
    if (s_auto_on) {
        s_auto_hold = true;
        s_auto_on   = false;
        if (s_auto_line != NULL) {
            lv_obj_add_flag(s_auto_line, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* 菜单取消时恢复原来的开关状态 */
static void auto_resume(void)
{
    if (s_auto_hold) {
        auto_set(true);
    }
}

/* ===================== 长按菜单 ===================== */

/* 跳转弹层（章节 / 页码）在下面实现，这里先声明 */
static void goto_show(void);

enum {
    ACT_COVER = 0,
    ACT_INTRO,
    ACT_GOTO,
    ACT_AUTO,
    ACT_AUTO_SEC,
};

static int s_menu_act[UI_MENU_MAX_ITEMS];
static char s_book_name[SRV_BOOK_NAME_MAX];

/* 翻页间隔档位：5 档正好塞满一个菜单（再多就超出 240 高的屏幕了） */
#define AUTO_SEC_N  5
static const int s_auto_choices[AUTO_SEC_N] = {10, 15, 20, 30, 60};

static void on_auto_sec_pick(int idx, void *arg)
{
    (void)arg;
    if (idx < 0 || idx >= AUTO_SEC_N) {
        return;
    }
    s_auto_sec = s_auto_choices[idx];
    srv_config_set_int(CFG_READ_AUTO_SEC, s_auto_sec, true);
    auto_set(true);                     /* 选完直接开始，省一次点击 */
}

static void auto_sec_menu_show(void)
{
    ui_menu_item_t items[AUTO_SEC_N];
    /* 16 字节装不下 "15 秒 · 当前"（中文字符 3 字节），GCC 的
     * -Wformat-truncation 会直接报错，这里按最大值留够 */
    char texts[AUTO_SEC_N][24];

    for (int i = 0; i < AUTO_SEC_N; i++) {
        if (s_auto_choices[i] == s_auto_sec) {
            snprintf(texts[i], sizeof(texts[i]), "%d 秒 · 当前", s_auto_choices[i]);
        } else {
            snprintf(texts[i], sizeof(texts[i]), "%d 秒", s_auto_choices[i]);
        }
        items[i].text = texts[i];
        items[i].icon = NULL;
    }
    /* 文本由 lv_label_set_text 复制，栈上的 texts 出了函数也没事 */
    ui_menu_show("翻页间隔", items, AUTO_SEC_N, on_auto_sec_pick, NULL);
}

static void act_show_cover(void)
{
    const srv_book_t *bk = NULL;
    if (srv_library_find(srv_reader_path(), &bk) != ESP_OK || bk == NULL) {
        return;
    }

    uint8_t *data  = NULL;
    size_t   bytes = 0;
    int32_t  w     = 0;
    int32_t  h     = 0;

    esp_err_t ret = srv_library_read_cover(bk, &data, &bytes, &w, &h);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "读取封面失败 %s: %s", srv_reader_path(), esp_err_to_name(ret));
        ui_alert_show("读取封面失败", UI_COLOR_WARN, s_book_name, "知道了", NULL, NULL, 2000);
        return;
    }

    /* 封面按物理竖屏铺满整屏显示，缓冲所有权交给弹层 */
    ui_cover_show(data, bytes, w, h);
}

static void act_show_intro(void)
{
    const srv_book_t *bk = NULL;
    if (srv_library_find(srv_reader_path(), &bk) != ESP_OK || bk == NULL) {
        return;
    }

    /* 与书架页同理：static 缓冲，别占 LVGL 任务栈 */
    static char intro[2048];
    esp_err_t ret = srv_library_read_intro(bk, intro, sizeof(intro));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "读取简介失败 %s: %s", srv_reader_path(), esp_err_to_name(ret));
        ui_alert_show("读取简介失败", UI_COLOR_WARN, s_book_name, "知道了", NULL, NULL, 2000);
        return;
    }

    ui_info_show(s_book_name, intro);
}

static void on_menu_pick(int idx, void *arg)
{
    (void)arg;
    if (idx < 0 || idx >= UI_MENU_MAX_ITEMS) {
        auto_resume();                  /* 取消：恢复菜单打开前的开关状态 */
        return;
    }

    switch (s_menu_act[idx]) {
    case ACT_COVER:
        s_auto_hold = false;            /* 看封面期间不自动翻页，回来也不自己恢复 */
        act_show_cover();
        break;
    case ACT_INTRO:
        s_auto_hold = false;
        act_show_intro();
        break;
    case ACT_GOTO:
        s_auto_hold = false;
        goto_show();
        break;
    case ACT_AUTO:
        /* 挂起时 s_auto_on 已被置 false，真实状态在 s_auto_hold 里 */
        auto_set(!(s_auto_on || s_auto_hold));
        break;
    case ACT_AUTO_SEC:
        auto_sec_menu_show();
        break;
    default:
        break;
    }
}

/* 长按正文（含左右点击带，靠事件冒泡传上来）弹菜单 */
static void on_reader_long(lv_event_t *e)
{
    (void)e;
    if (!srv_reader_is_open() || s_text_lb == NULL) {
        return;
    }

    s_menu_tick = lv_tick_get();        /* 让翻页回调吞掉稍后那次 CLICKED */
    snprintf(s_book_name, sizeof(s_book_name), "%s", srv_reader_title());

    ui_menu_item_t items[UI_MENU_MAX_ITEMS];
    int n = 0;

    /* 封面、简介都来自书架扫描到的元数据：书不在书架里（比如以后从文件管理器
     * 直接打开）就查不到，这时只留「跳转」 */
    const srv_book_t *bk = NULL;
    if (srv_library_find(srv_reader_path(), &bk) == ESP_OK && bk != NULL) {
        if (bk->type == BOOK_TYPE_EBF && bk->cover_bytes > 0) {
            items[n].text = "查看封面";
            items[n].icon = &ic_cover;
            s_menu_act[n++] = ACT_COVER;
        }
        if (bk->type == BOOK_TYPE_EBF && bk->intro[0] != '\0') {
            items[n].text = "查看简介";
            items[n].icon = &ic_info;
            s_menu_act[n++] = ACT_INTRO;
        }
    }

    if (srv_reader_format() == READER_FMT_EBF) {
        items[n].text = "跳转章节";
        items[n].icon = &ic_list;
    } else {
        items[n].text = "跳转页码";
        items[n].icon = &ic_page;
    }
    s_menu_act[n++] = ACT_GOTO;

    items[n].text = (s_auto_on || s_auto_hold) ? "停止自动翻页" : "自动翻页";
    items[n].icon = &ic_play;
    s_menu_act[n++] = ACT_AUTO;

    /* 间隔最大 600，加上「翻页间隔」「秒」共约 21 字节，留足余量 */
    static char sec_text[32];
    snprintf(sec_text, sizeof(sec_text), "翻页间隔 %d 秒", s_auto_sec);
    items[n].text = sec_text;
    items[n].icon = &ic_timer;
    s_menu_act[n++] = ACT_AUTO_SEC;

    /* 菜单弹出期间先挂起自动翻页，避免菜单还开着页面自己翻走 */
    auto_suspend();
    ui_menu_show(s_book_name, items, n, on_menu_pick, NULL);
}

/* ===================== 滑动手势 / 轻点翻页 ===================== */

/*! 横向位移超过这个像素才算翻页手势，避免手抖误翻 */
#define RD_SWIPE_MIN  40
/*! 位移在这个范围内才算「轻点」而不是「滑动」 */
#define RD_TAP_MAX    20
/*! 左右两侧多宽算「上一页 / 下一页」点击区 */
#define RD_BAND_W     (UI_SCREEN_W / 3)

static lv_point_t s_slide_start;
static bool       s_pressed;

static void reader_pressed_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }
    lv_indev_get_point(indev, &s_slide_start);
    s_pressed = true;
}

static void reader_press_lost_cb(lv_event_t *e)
{
    (void)e;
    s_pressed = false;
}

/**
 * @brief 松手时统一判定：位移够大按方向翻页，没怎么动就按落点翻页
 *
 * 两个动作都放在这里判定，透明点击带不再自己收 CLICKED —— 否则在带上
 * 滑动会同时触发「手势翻页」和「点击翻页」，一次手势翻两页（LVGL 释放时
 * 是无条件发 CLICKED 的，滑动并不会把它取消）。
 * 判定只看按下的起点，整屏任意位置起手都能滑动翻页。
 */
static void reader_released_cb(lv_event_t *e)
{
    (void)e;
    if (!s_pressed) {
        return;
    }
    s_pressed = false;

    /* 长按弹完菜单松手时 LVGL 还会补一次释放，别让它顺手翻页 */
    if (swallow_click_after_long()) {
        return;
    }

    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    int dx = p.x - s_slide_start.x;
    int dy = p.y - s_slide_start.y;

    /* 滑动翻页：横向为主且够远 */
    if (abs(dx) >= RD_SWIPE_MIN && abs(dx) >= abs(dy) * 2) {
        if (dx < 0) {
            turn_next();                /* 左滑 → 下一页（到章末就进下一章） */
        } else {
            turn_prev();                /* 右滑 → 上一页（到章首就退上一章末页） */
        }
        return;
    }

    /* 轻点：按落点判断是不是点在左右两条翻页带上，中间不响应 */
    if (abs(dx) < RD_TAP_MAX && abs(dy) < RD_TAP_MAX) {
        if (s_slide_start.x < RD_BAND_W) {
            turn_prev();
        } else if (s_slide_start.x > UI_SCREEN_W - RD_BAND_W) {
            turn_next();
        }
    }
}

/* ===================== 跳转弹层（章节 / 页码） ===================== */

#define GOTO_PANEL_W    300
#define GOTO_PANEL_H    216
#define GOTO_LIST_W     168
#define GOTO_LIST_H     172
#define GOTO_KEY_GAP    4
/*! 键盘能输几位数字：章数不设上限，所以位数在开弹层时按实际章数算 */
#define GOTO_DIGITS_MAX 10
#define GOTO_DIGITS_BUF (GOTO_DIGITS_MAX + 1)

/* ===================== 章节列表：自建虚拟列表 =====================
 *
 * 原来用 lv_roller_set_options 把**整本书的章名**拼成一个大字符串塞进去，
 * roller 内部要对这串文本整体做行高/换行计算，几千章就是 O(n) 的文本处理 ——
 * 打开弹层要等数百毫秒到几秒，滑动时也要在大文本里定位。
 *
 * 改成虚拟列表：只建 GOTO_ROWS 个行对象，任何时候屏幕上显示的都是这十几个对象，
 * 滚动/输入只是「按索引改文本 + 挪坐标」，任何一次操作都是 O(1)，与章数无关。
 *
 * 滚动是自己管的（容器**没有** SCROLLABLE 标志）：行的坐标由 s_goto_pos 换算，
 * 这样不受 LVGL 滚动条/弹性/链条那一套影响，也不必给列表造一个虚拟高度。
 */
#define GOTO_ROW_H      28      /*!< 每行高度 */
/*! 同时存在的行数：可见 6 行（172/28）+ 2 行缓冲，滚动中途不会露白 */
#define GOTO_ROWS       (GOTO_LIST_H / GOTO_ROW_H + 2)

static lv_obj_t *s_goto_mask;
static lv_obj_t *s_goto_input;          /*!< 顶部输入显示 */
static lv_obj_t *s_goto_list;           /*!< EBF 的章节虚拟列表容器；TXT 为 NULL */
static char      s_goto_digits[GOTO_DIGITS_BUF];
static int       s_goto_digit_n;
static int       s_goto_input_max = 4;  /*!< 本次允许输入的位数 */

static lv_obj_t *s_goto_rows[GOTO_ROWS];    /*!< 复用的行对象（行高亮 + 文字） */
static lv_obj_t *s_goto_row_lbs[GOTO_ROWS]; /*!< 每行里的标签 */
static int       s_goto_row_idx[GOTO_ROWS]; /*!< 该行当前显示的章索引，-1 = 空行 */
static int       s_goto_cnt;                /*!< 章节总数（开弹层时取一次，之后只读） */
static int       s_goto_sel;                /*!< 当前选中的章索引 */
static int       s_goto_top;                /*!< 窗口顶部对应的章索引（刷新时算出来） */
static int32_t   s_goto_pos;                /*!< 顶部相对第 0 章的像素偏移 */
static int32_t   s_goto_pos_max;             /*!< 偏移上限：滚到最后一章 */
static bool      s_goto_dragging;           /*!< 本次触摸是拖动（用来区分点击与滑动） */
static bool      s_goto_moved;              /*!< 本次触摸有没有真的拖动过 */
static bool      s_goto_refreshing;         /*!< 刷新重入保护 */

/* 十进制位数：1 → 1，10 → 2（数章节号/页码要几位） */
static int dec_digits(int v)
{
    int n = 1;
    while (v >= 10) {
        v /= 10;
        n++;
    }
    return n;
}

static void goto_close(void);
static void goto_input_refresh(void);
/* 换章后要改标题栏（EBF 显示当前章名），实现在下面的标题栏那一节 */
static void reader_update_title(void);

static void goto_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    if (lv_event_get_target(e) != s_goto_mask) {
        return;                         /* 已经是新的弹层了，别清 */
    }
    s_goto_mask    = NULL;
    s_goto_input   = NULL;
    s_goto_list    = NULL;              /* 行对象随容器一起被删掉，这里只清指针 */
    s_goto_digit_n = 0;
    s_goto_digits[0] = '\0';

    for (int i = 0; i < GOTO_ROWS; i++) {
        s_goto_rows[i]    = NULL;
        s_goto_row_lbs[i] = NULL;
        s_goto_row_idx[i] = -1;
    }
    s_goto_dragging = false;
    s_goto_moved    = false;
    s_goto_pos      = 0;
}

static void goto_close(void)
{
    if (s_goto_mask != NULL && lv_obj_is_valid(s_goto_mask)) {
        lv_obj_delete(s_goto_mask);
    }
}

static void goto_cancel_cb(lv_event_t *e)
{
    (void)e;
    goto_close();
}

/* ===================== 章节虚拟列表：滚动与刷新 ===================== */

/* 滚出列表范围的行由 LVGL 自动裁掉（子对象默认被父对象裁剪），不用自己藏 */

static void goto_scroll_clamp(void)
{
    if (s_goto_pos < 0) {
        s_goto_pos = 0;
    }
    if (s_goto_pos > s_goto_pos_max) {
        s_goto_pos = s_goto_pos_max;
    }
}

/**
 * @brief 按当前 s_goto_pos 刷新这几行的文字、位置和高亮
 *
 * 只在「滚动/定位/选中变了」时调，一次最多动 GOTO_ROWS 个对象，
 * 跟章节总数无关 —— 这是打开和滑动都不卡的关键。
 */
static void goto_list_refresh(void)
{
    if (s_goto_list == NULL || s_goto_refreshing) {
        return;
    }
    s_goto_refreshing = true;

    int top  = (int)(s_goto_pos / GOTO_ROW_H);
    int frac = (int)(s_goto_pos - (int32_t)top * GOTO_ROW_H);
    if (top < 0) {
        top  = 0;
        frac = 0;
    }
    s_goto_top = top;

    char title[CHAP_TITLE_MAX];
    char buf[CHAP_TITLE_MAX + 16];

    for (int i = 0; i < GOTO_ROWS; i++) {
        lv_obj_t *row = s_goto_rows[i];
        int idx = top + i;

        if (idx >= 0 && idx < s_goto_cnt) {
            srv_reader_chapter_title(idx, title, sizeof(title));
            snprintf(buf, sizeof(buf), "%d. %s", idx + 1, title);
            lv_label_set_text(s_goto_row_lbs[i], buf);
            /* 位置是相对列表内容区的手动换算：第 i 行落在 i*行高 - 余数 */
            lv_obj_set_pos(row, 0, (lv_coord_t)(i * GOTO_ROW_H - frac));
            lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
            s_goto_row_idx[i] = idx;
        } else {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            s_goto_row_idx[i] = -1;
        }

        bool sel = (s_goto_row_idx[i] >= 0 && s_goto_row_idx[i] == s_goto_sel);
        lv_obj_set_style_bg_opa(row, sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(s_goto_row_lbs[i],
                                   sel ? lv_color_white() : UI_COLOR_TEXT_DIM, 0);
    }

    s_goto_refreshing = false;
}

/* 把选中章滚到窗口中间（键盘输章节号之后用） */
static void goto_scroll_to_sel(void)
{
    if (s_goto_list == NULL) {
        return;
    }
    int vis = GOTO_LIST_H / GOTO_ROW_H;     /* 一屏几行 */
    int32_t top = (int32_t)s_goto_sel - vis / 2;
    if (top < 0) {
        top = 0;
    }
    s_goto_pos = top * GOTO_ROW_H;
    goto_scroll_clamp();
}

static void goto_list_pressed(lv_event_t *e)
{
    (void)e;
    s_goto_dragging = true;
    s_goto_moved    = false;            /* 每次按下都重新判定 */
}

static void goto_list_pressing(lv_event_t *e)
{
    (void)e;
    if (!s_goto_dragging) {
        return;
    }

    /* vect 是「相对上一次事件」的位移，累加到 s_goto_pos 上比记绝对坐标省事，
     * 也不会因为手指滑出列表区域而跳变 */
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }
    lv_point_t v;
    lv_indev_get_vect(indev, &v);
    if (v.y == 0) {
        return;
    }

    s_goto_moved = true;
    s_goto_pos  -= v.y;                 /* 手指往下拉 → 内容往上走，看到更前面的章 */
    goto_scroll_clamp();
    goto_list_refresh();
}

static void goto_list_released(lv_event_t *e)
{
    (void)e;
    s_goto_dragging = false;

    /* 拖动过就吸附到整行；没拖动的交给 CLICKED 当点击处理 */
    if (!s_goto_moved) {
        return;
    }
    s_goto_pos = (s_goto_pos + GOTO_ROW_H / 2) / GOTO_ROW_H * GOTO_ROW_H;
    goto_scroll_clamp();
    goto_list_refresh();
}

/* 点一行 = 选中它（再点「确定」才真的跳章，跟原来的滚轮行为一致） */
static void goto_list_clicked(lv_event_t *e)
{
    (void)e;

    if (s_goto_moved) {
        s_goto_moved = false;           /* 这是拖动收尾带来的点击，忽略 */
        return;
    }

    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL || s_goto_list == NULL) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);      /* 屏幕坐标 */
    lv_area_t a;
    lv_obj_get_coords(s_goto_list, &a);

    int y    = (int)(p.y - a.y1);
    int frac = (int)(s_goto_pos - (int32_t)s_goto_top * GOTO_ROW_H);
    int i    = (y + frac) / GOTO_ROW_H;
    if (i < 0) {
        i = 0;
    }
    if (i >= GOTO_ROWS || s_goto_row_idx[i] < 0) {
        return;
    }

    s_goto_sel = s_goto_row_idx[i];
    goto_list_refresh();
    goto_input_refresh();
}

/* 输入框显示：没输入时显示当前定位（EBF 是列表选中项，TXT 是当前页） */
static void goto_input_refresh(void)
{
    if (s_goto_input == NULL) {
        return;
    }

    char buf[32];
    if (s_goto_digit_n > 0) {
        snprintf(buf, sizeof(buf), "第 %s %s", s_goto_digits,
                 (s_goto_list != NULL) ? "章" : "页");
    } else if (s_goto_list != NULL) {
        snprintf(buf, sizeof(buf), "第 %d 章", s_goto_sel + 1);
    } else {
        snprintf(buf, sizeof(buf), "第 %d 页", s_page_cur + 1);
    }
    lv_label_set_text(s_goto_input, buf);
}

static void goto_chapter_jump(int idx)
{
    goto_close();                       /* 先撤弹层再重排，免得压在旧内容上 */

    /* 切章 + 重排页表 + 刷新章名都在 chapter_jump 里，翻页跨章走的是同一套 */
    if (chapter_jump(idx, false)) {
        ESP_LOGI(TAG, "跳到第 %d 章", idx + 1);
    }
}

static void goto_page_jump(int page)
{
    if (page < 1) {
        return;
    }

    goto_close();
    show_page(page - 1);                /* 会一页页推导到目标页 */

    if (s_page_cur != page - 1) {
        /* 推导到书尾也没走到这一页：页码超了 */
        ui_alert_show("页码超出范围", UI_COLOR_WARN, srv_reader_title(),
                      "知道了", NULL, NULL, 1800);
    }
}

/* EBF：输入数字就是「在列表里定位」，不是直接进去 */
static void goto_locate(void)
{
    if (s_goto_list == NULL) {
        return;
    }

    int cnt = s_goto_cnt;               /* 开弹层时取的，之后只读 */
    if (cnt <= 0) {
        return;
    }

    /* 用 strtol 而不是 atoi：现在最多能输 10 位，atoi 溢出是未定义行为，
     * strtol 溢出会饱和到 LONG_MAX，夹一下就行 */
    long v = (s_goto_digit_n > 0) ? strtol(s_goto_digits, NULL, 10) : 0;
    if (v < 1) {
        v = 1;
    }
    if (v > cnt) {
        v = cnt;
    }

    s_goto_sel = (int)(v - 1);
    goto_scroll_to_sel();               /* 把这一章滚到窗口中间 */
    goto_list_refresh();
}

static void goto_digit_cb(lv_event_t *e)
{
    char d = (char)(intptr_t)lv_event_get_user_data(e);

    if (s_goto_digit_n < s_goto_input_max) {
        s_goto_digits[s_goto_digit_n++] = d;
        s_goto_digits[s_goto_digit_n] = '\0';
    }
    goto_locate();
    goto_input_refresh();
}

static void goto_back_cb(lv_event_t *e)
{
    (void)e;
    if (s_goto_digit_n > 0) {
        s_goto_digits[--s_goto_digit_n] = '\0';
    }
    goto_locate();
    goto_input_refresh();
}

static void goto_ok_cb(lv_event_t *e)
{
    (void)e;

    if (s_goto_list != NULL) {
        /* 章节号在输入时已经定位到列表上了，这里就是确认进去 */
        goto_chapter_jump(s_goto_sel);
        return;
    }

    if (s_goto_digit_n > 0) {
        goto_page_jump(atoi(s_goto_digits));
    }
}

static void goto_add_key(lv_obj_t *box, lv_coord_t x, lv_coord_t y,
                         lv_coord_t w, lv_coord_t h, const char *text,
                         lv_event_cb_t cb, void *arg, bool primary)
{
    lv_obj_t *btn = lv_button_create(box);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, primary ? UI_COLOR_PRIMARY : UI_COLOR_BG, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    ui_add_click_feedback(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, arg);

    lv_obj_t *lb = lv_label_create(btn);
    lv_obj_set_style_text_font(lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(lb, primary ? lv_color_white() : UI_COLOR_TEXT, 0);
    lv_label_set_text(lb, text);
    lv_obj_center(lb);
}

/* 3x4 小键盘：1~9 / 退格 0 确定 */
static void goto_build_keypad(lv_obj_t *box, lv_coord_t x0, lv_coord_t y0,
                              lv_coord_t kw, lv_coord_t kh)
{
    for (int i = 0; i < 12; i++) {
        lv_coord_t x = x0 + (lv_coord_t)(i % 3) * (kw + GOTO_KEY_GAP);
        lv_coord_t y = y0 + (lv_coord_t)(i / 3) * (kh + GOTO_KEY_GAP);

        if (i < 9) {
            char t[2] = { (char)('1' + i), '\0' };
            goto_add_key(box, x, y, kw, kh, t, goto_digit_cb,
                         (void *)(intptr_t)('1' + i), false);
        } else if (i == 9) {
            goto_add_key(box, x, y, kw, kh, "退格", goto_back_cb, NULL, false);
        } else if (i == 10) {
            goto_add_key(box, x, y, kw, kh, "0", goto_digit_cb,
                         (void *)(intptr_t)'0', false);
        } else {
            goto_add_key(box, x, y, kw, kh, "确定", goto_ok_cb, NULL, true);
        }
    }
}

/* EBF 的章节列表：自建虚拟列表（常量与取舍见上面的长注释）。
 *
 * 只建 GOTO_ROWS 个行对象，滚动/定位时复用改文本 —— 打开与滑动都是 O(1)，
 * 与章节总数无关。行对象刻意**不带 CLICKABLE**，按压/拖动事件统一落到容器上，
 * 免得手指压在某一行的文字上时事件目标在行与容器之间来回跳。 */
static bool goto_build_chapters(lv_obj_t *box)
{
    int cnt = srv_reader_chapter_count();
    if (cnt <= 0) {
        return false;
    }

    lv_obj_t *list = lv_obj_create(box);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);   /* 滚动自己管，不用 LVGL 那套 */
    lv_obj_add_flag(list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(list, GOTO_LIST_W, GOTO_LIST_H);
    lv_obj_set_pos(list, 8, 36);
    lv_obj_set_style_bg_color(list, UI_COLOR_BG, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 8, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_add_event_cb(list, goto_list_pressed,  LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(list, goto_list_pressing, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(list, goto_list_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(list, goto_list_clicked,  LV_EVENT_CLICKED,  NULL);

    s_goto_list = list;
    s_goto_cnt  = cnt;

    int cur = srv_reader_chapter_index();
    if (cur < 0 || cur >= cnt) {
        cur = 0;
    }
    s_goto_sel = cur;

    /* 行对象只在这里建一次，之后全程复用；超出列表范围的行由 LVGL 自动裁剪 */
    for (int i = 0; i < GOTO_ROWS; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);  /* 按压落到容器上 */
        lv_obj_set_size(row, GOTO_LIST_W, GOTO_ROW_H);
        lv_obj_set_pos(row, 0, (lv_coord_t)(i * GOTO_ROW_H));
        lv_obj_set_style_bg_color(row, UI_COLOR_PRIMARY, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_left(row, 8, 0);
        lv_obj_set_style_pad_right(row, 4, 0);

        lv_obj_t *lb = lv_label_create(row);
        lv_obj_set_style_text_font(lb, &font_ui_14, 0);
        lv_obj_set_style_text_color(lb, UI_COLOR_TEXT_DIM, 0);
        /* 长章名循环滚动看全称（行高只有一行，折行会压到下一行） */
        lv_label_set_long_mode(lb, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_anim_duration(lb, 6000, 0);
        lv_obj_set_width(lb, GOTO_LIST_W - 12);
        lv_obj_set_height(lb, font_ui_14.line_height);
        lv_label_set_text(lb, "");
        lv_obj_align(lb, LV_ALIGN_LEFT_MID, 0, 0);

        s_goto_rows[i]    = row;
        s_goto_row_lbs[i] = lb;
        s_goto_row_idx[i] = -1;
    }

    /* 偏移上限：滚到底时最后一行刚好吃满列表底边 */
    s_goto_pos_max = cnt * GOTO_ROW_H - GOTO_LIST_H;
    if (s_goto_pos_max < 0) {
        s_goto_pos_max = 0;
    }

    goto_scroll_to_sel();               /* 开机定位到正在读的章 */
    goto_list_refresh();
    return true;
}

static void goto_show(void)
{
    if (!srv_reader_is_open() || s_text_lb == NULL || s_goto_mask != NULL) {
        return;
    }

    lv_obj_t *scr = lv_screen_active();
    if (scr == NULL) {
        return;
    }

    /* 自建遮罩：这一层要跟阅读器共用 show_page / 页表，放 ui_dialog 里反而绕 */
    lv_obj_t *mask = lv_obj_create(scr);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(mask, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_60, 0);
    lv_obj_set_style_border_width(mask, 0, 0);
    lv_obj_set_style_radius(mask, 0, 0);
    lv_obj_set_style_pad_all(mask, 0, 0);

    s_goto_mask    = mask;
    s_goto_digit_n = 0;
    s_goto_digits[0] = '\0';
    lv_obj_add_event_cb(mask, goto_del_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(mask, goto_cancel_cb, LV_EVENT_CLICKED, NULL);   /* 点空白 = 取消 */

    lv_obj_t *box = lv_obj_create(mask);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(box, GOTO_PANEL_W, GOTO_PANEL_H);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 0, 0);

    /* 顶部：输入显示 + 取消 */
    s_goto_input = lv_label_create(box);
    lv_obj_set_style_text_font(s_goto_input, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_goto_input, UI_COLOR_TEXT, 0);
    lv_obj_set_width(s_goto_input, 200);
    lv_obj_set_pos(s_goto_input, 12, 8);
    lv_label_set_text(s_goto_input, "");

    lv_obj_t *cancel = lv_button_create(box);
    lv_obj_set_size(cancel, 52, 24);
    lv_obj_set_pos(cancel, GOTO_PANEL_W - 60, 5);
    lv_obj_set_style_radius(cancel, 6, 0);
    lv_obj_set_style_bg_color(cancel, UI_COLOR_BG, 0);
    lv_obj_set_style_shadow_width(cancel, 0, 0);
    lv_obj_set_style_pad_all(cancel, 0, 0);
    ui_add_click_feedback(cancel);
    lv_obj_add_event_cb(cancel, goto_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *clb = lv_label_create(cancel);
    lv_obj_set_style_text_font(clb, &font_ui_14, 0);
    lv_obj_set_style_text_color(clb, UI_COLOR_TEXT, 0);
    lv_label_set_text(clb, "取消");
    lv_obj_center(clb);

    if (srv_reader_format() == READER_FMT_EBF) {
        if (!goto_build_chapters(box)) {
            goto_close();
            ui_alert_show("章节列表打不开", UI_COLOR_WARN, "内存不足", "知道了", NULL, NULL, 1800);
            return;
        }
        /* 键盘允许输入的位数跟着章数走：章数不设上限，写死 4 位的话
         * 一万章以上的书就没法用键盘输章节号了 */
        s_goto_input_max = dec_digits(srv_reader_chapter_count());
        if (s_goto_input_max < 1) {
            s_goto_input_max = 1;
        } else if (s_goto_input_max > GOTO_DIGITS_MAX) {
            s_goto_input_max = GOTO_DIGITS_MAX;
        }
        goto_build_keypad(box, 182, 36, 34, 40);
    } else {
        /* TXT 只有页码，键盘放大居中，按起来宽敞些。
         * 页表本身有 PAGE_MAX 上限，页码 4 位（9999）已经远超能翻到的范围 */
        s_goto_input_max = 4;
        goto_build_keypad(box, 80, 64, 44, 34);
    }

    goto_input_refresh();
}

/* ===================== 等待层（打开中 / 排版中） ===================== */

static void reader_build_content(lv_obj_t *root);
static void reader_resume(lv_obj_t *root);
static void reader_enter(lv_obj_t *root);

/**
 * @brief 确保标题栏存在
 *
 * 标题栏必须**先于正文**建、而且加载期间也得在：阅读器没有手势返回，
 * 退出全靠标题栏上那个返回键，晚建或漏建就等于把用户关在书里出不来。
 * 做成「没有才建」是因为书的标题要等打开完才拿得到，那时只需要改字。
 */
static void reader_titlebar_ensure(lv_obj_t *root, const char *title)
{
    if (s_titlebar == NULL) {
        s_titlebar = ui_titlebar_create(root, title, app_navigator_back_cb, NULL);
        /* 阅读器的章名/书名不滚动：正文就在下面，标题一直动会分心 */
        ui_titlebar_set_scroll(s_titlebar, false);
    }
}

/**
 * @brief 刷新标题栏：EBF 显示当前章名，TXT 显示文件名
 *
 * 章名来自章节文件首部（当前章）——打开这一章时已经解析过了，这里不产生 IO。
 * 取不到就退回"第 N 章"。
 */
static void reader_update_title(void)
{
    if (s_titlebar == NULL) {
        return;
    }

    char buf[CHAP_TITLE_MAX];
    if (srv_reader_format() == READER_FMT_EBF) {
        srv_reader_chapter_title(srv_reader_chapter_index(), buf, sizeof(buf));
    } else {
        snprintf(buf, sizeof(buf), "%s", srv_reader_title());
    }
    ui_titlebar_set_title(s_titlebar, buf);
}

static void wait_layer_close(void)
{
    if (s_wait_layer != NULL) {
        lv_obj_delete(s_wait_layer);
        s_wait_layer = NULL;
        s_wait_note = NULL;
        s_wait_bar = NULL;
        s_wait_pct = NULL;
    }
}

static void wait_layer_note(const char *note)
{
    /* 值没变就别 set —— 打开阶段每 50ms 来一次，白刷会引起无谓重绘 */
    if (s_wait_note != NULL && strcmp(lv_label_get_text(s_wait_note), note) != 0) {
        lv_label_set_text(s_wait_note, note);
    }
}

/**
 * @brief 更新进度条（0-100）
 *
 * 两点讲究：用 LV_ANIM_OFF（动画会一直重绘，慢的时候反而更卡，用户也
 * 不喜欢多余动效）；值没变就直接返回，别白刷一遍控件。
 */
static void wait_layer_progress(int pct)
{
    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }

    if (s_wait_bar != NULL && (int)lv_bar_get_value(s_wait_bar) != pct) {
        lv_bar_set_value(s_wait_bar, pct, LV_ANIM_OFF);
    }

    if (s_wait_pct != NULL) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", pct);
        if (strcmp(lv_label_get_text(s_wait_pct), buf) != 0) {
            lv_label_set_text(s_wait_pct, buf);
        }
    }
}

/**
 * @brief 铺一层「请稍候」：盖住标题栏以下的区域，中间一块面板
 *
 * 只盖标题栏以下：标题栏里的返回键是加载期间唯一的退路（阅读器没有
 * 手势返回），盖住它用户就只能干等。层本身可点击，顺带吞掉底下还没
 * 建好的翻页手势区。
 */
static void wait_layer_create(lv_obj_t *root, const char *title, int pct)
{
    wait_layer_close();

    s_wait_layer = lv_obj_create(root);
    lv_obj_remove_flag(s_wait_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_wait_layer, UI_SCREEN_W, UI_SCREEN_H - UI_TITLEBAR_H);
    lv_obj_set_pos(s_wait_layer, 0, UI_TITLEBAR_H);
    lv_obj_set_style_bg_color(s_wait_layer, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_wait_layer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_wait_layer, 0, 0);
    lv_obj_set_style_radius(s_wait_layer, 0, 0);
    lv_obj_set_style_pad_all(s_wait_layer, 0, 0);
    lv_obj_add_flag(s_wait_layer, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *panel = lv_obj_create(s_wait_layer);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(panel, 220, 96);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(panel, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);

    lv_obj_t *lb = lv_label_create(panel);
    lv_obj_set_style_text_color(lb, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(lb, &font_ui_14, 0);
    lv_label_set_text(lb, title);
    lv_obj_align(lb, LV_ALIGN_TOP_MID, 0, 0);

    s_wait_note = lv_label_create(panel);
    lv_obj_set_style_text_color(s_wait_note, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_wait_note, &font_ui_14, 0);
    lv_label_set_text(s_wait_note, "");
    lv_obj_align(s_wait_note, LV_ALIGN_TOP_MID, 0, 20);

    /* 进度条：轨道深灰、指示器是主题色，中间叠一个百分比。
     * 条高给了 20 —— 14px 的字行高就 17 左右，条太矮文字会被裁掉一截 */
    s_wait_bar = lv_bar_create(panel);
    lv_obj_remove_flag(s_wait_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_wait_bar, 180, 20);
    lv_obj_align(s_wait_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_bar_set_range(s_wait_bar, 0, 100);
    lv_obj_set_style_pad_all(s_wait_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_wait_bar, lv_color_hex(0x2A313A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_wait_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_wait_bar, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_wait_bar, UI_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_wait_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_wait_bar, 8, LV_PART_INDICATOR);

    s_wait_pct = lv_label_create(s_wait_bar);
    lv_obj_set_style_text_color(s_wait_pct, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_wait_pct, &font_ui_14, 0);
    lv_label_set_text(s_wait_pct, "0%");
    lv_obj_center(s_wait_pct);

    wait_layer_progress(pct);
}

/* ===================== 打开 / 排版 ===================== */

/*! 一个 tick 排多少页：太小拖慢恢复，太大又会让界面一顿一顿的 */
#define BUILD_PAGES_PER_TICK  12

/**
 * @brief 页面销毁时清掉 static 指针与定时器
 *
 * 必须比对归属：切页用的是异步删除，如果用户手快地「阅读器→书架→阅读器」，
 * 新页面的对象可能已经建好，这时旧页面的销毁回调再无脑置 NULL，
 * 就会把新页面的指针清掉（定时器同理，删错了会留下野回调）。
 */
static void reader_root_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }

    /* 页面销毁 = 退出阅读器（回书架 / 回时钟），在这里补一次强制保存：
     * 翻页时的保存有最小写盘间隔，最后翻的那几页会被节流挡掉，不补就丢了。
     * 放在清指针之前、也不依赖任何 UI 对象 —— 手快地「换书再退」时 static
     * 已经指向新页面，但这几行读的是 srv_reader 的状态，存的是当前这本书的位置。 */
    if (srv_reader_is_open()) {
        srv_reader_save_progress_force();
    }

    lv_obj_t *root = lv_event_get_target(e);

    if (s_open_poll != NULL && lv_timer_get_user_data(s_open_poll) == root) {
        lv_timer_delete(s_open_poll);
        s_open_poll = NULL;
    }
    if (s_build_timer != NULL && lv_timer_get_user_data(s_build_timer) == root) {
        lv_timer_delete(s_build_timer);
        s_build_timer = NULL;
    }
    if (s_wait_layer != NULL && lv_obj_get_parent(s_wait_layer) == root) {
        s_wait_layer = NULL;
        s_wait_note = NULL;
        s_wait_bar = NULL;
        s_wait_pct = NULL;
    }
    if (s_titlebar != NULL && lv_obj_get_parent(s_titlebar) == root) {
        s_titlebar = NULL;
    }

    if (s_text_lb != NULL && lv_obj_get_parent(s_text_lb) == root) {
        s_text_lb = NULL;
        s_info_lb = NULL;
        s_measure = NULL;
    }

    /* 自动翻页：定时器按 root 认领，线对象随页面一起销毁 */
    if (s_auto_timer != NULL && lv_timer_get_user_data(s_auto_timer) == root) {
        lv_timer_delete(s_auto_timer);
        s_auto_timer = NULL;
    }
    if (s_auto_line != NULL && lv_obj_get_parent(s_auto_line) == root) {
        s_auto_line = NULL;
    }
    s_auto_on   = false;
    s_auto_hold = false;
}

/**
 * @brief 页表推到目标位置（或推到书尾）后收尾：定位当前页并画出来
 */
static void layout_finish(void)
{
    for (int i = 0; i < s_page_cnt; i++) {
        if (s_page_start[i] <= s_build_target) {
            s_page_cur = i;
        } else {
            break;
        }
    }
    show_page(s_page_cur);

    ESP_LOGI(TAG, "排版完成：第 %d 页，字偏移 %ld，共 %d 页",
             s_page_cur + 1, (long)s_page_start[s_page_cur], s_page_cnt);
}

/**
 * @brief 分片建页表：每次排一小批，中间让 LVGL 有机会刷屏
 */
static void build_timer_cb(lv_timer_t *t)
{
    bool stopped = false;

    for (int i = 0; i < BUILD_PAGES_PER_TICK; i++) {
        if (s_page_start[s_page_cnt - 1] >= s_build_target) {
            break;                      /* 已经盖到目标位置 */
        }
        if (!build_next_page()) {
            stopped = true;             /* 书到头了，或者页表满了 */
            break;
        }
    }

    if (!stopped && s_page_start[s_page_cnt - 1] < s_build_target) {
        int32_t target = (s_build_target > 0) ? s_build_target : 1;
        int local = (int)((int64_t)s_page_start[s_page_cnt - 1] * 100 / target);
        if (local > 100) {
            local = 100;
        }
        /* 排版占后 60%，接着「打开」那一段继续往上走，不回头重来 */
        wait_layer_progress(WAIT_OPEN_PCT + local * (100 - WAIT_OPEN_PCT) / 100);
        return;
    }

    lv_timer_delete(t);
    if (s_build_timer == t) {
        s_build_timer = NULL;
    }
    wait_layer_close();
    layout_finish();
}

/**
 * @brief 轮询后台打开的结果
 */
static void open_poll_cb(lv_timer_t *t)
{
    if (srv_reader_busy()) {
        int done = 0;
        int total = 0;
        srv_reader_open_bytes(&done, &total);

        if (total > 0) {
            /* 正在读正文：把「已读 / 总共」摆出来，比光一个百分比实在。
             * 还没开始读正文（读元数据那会儿）total 是 0，保持原来的提示语。 */
            char note[40];
            snprintf(note, sizeof(note), "已读 %d KB / %d KB", done / 1024, total / 1024);
            wait_layer_note(note);
        }

        /* 打开阶段的进度由 srv_reader 报（正文是分块读的），折算到前 40% */
        wait_layer_progress(srv_reader_open_progress() * WAIT_OPEN_PCT / 100);
        return;
    }

    lv_obj_t *root = (lv_obj_t *)lv_timer_get_user_data(t);
    esp_err_t ret = srv_reader_open_result();

    lv_timer_delete(t);
    if (s_open_poll == t) {
        s_open_poll = NULL;
    }
    wait_layer_close();

    if (ret != ESP_OK) {
        /* 页面上直接摆错误，别用弹层 —— 弹层一关用户就困在一片空白上了。
         * 标题栏在加载时就已经建好（带返回键），这里只改一下标题。 */
        ESP_LOGW(TAG, "打开失败: %s", esp_err_to_name(ret));
        ui_titlebar_set_title(s_titlebar, "阅读");

        lv_obj_t *lb = lv_label_create(root);
        lv_obj_set_style_text_color(lb, UI_COLOR_WARN, 0);
        lv_obj_set_style_text_font(lb, &font_ui_14, 0);
        lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text_fmt(lb, "打不开这本书\n%s", esp_err_to_name(ret));
        lv_obj_align(lb, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    /* 书名这时才拿得到，标题栏的文字交给 reader_enter 里的 reader_update_title
     * 按书/章名分别处理 */
    reader_enter(root);
}

/**
 * @brief 开始重建页表
 *
 * 先同步推几页：刚打开的书目标位置一般在开头，一下就到位，不用闪遮罩。
 * 目标在很后面（恢复上次读到的位置）才换成分片模式，边排边报进度。
 */
static void reader_start_layout(lv_obj_t *root, int target)
{
    s_page_start[0] = 0;
    s_page_cnt = 1;
    s_page_cur = 0;
    s_build_target = target;

    while (s_page_cnt < 8 && s_page_start[s_page_cnt - 1] < target) {
        if (!build_next_page()) {
            break;
        }
    }

    if (s_page_start[s_page_cnt - 1] >= target) {
        layout_finish();
        return;
    }

    wait_layer_create(root, "正在排版", WAIT_OPEN_PCT);
    wait_layer_note("恢复阅读位置…");
    s_build_timer = lv_timer_create(build_timer_cb, 20, root);
    if (s_build_timer == NULL) {
        layout_finish();                /* 定时器都建不出来就别等了，直接给一页 */
    }
}

/* ===================== 建页 ===================== */

/**
 * @brief 建正文控件、手势与事件（标题栏由调用方单独建）
 */
static void reader_build_content(lv_obj_t *root)
{
    /* 排版参数每次进页面都重读：改完「设置 → 阅读」再进来就生效，
     * 重建页表也会按新参数来 */
    read_style_load();

    /* 正文 */
    s_text_lb = lv_label_create(root);
    lv_obj_set_style_text_color(s_text_lb, UI_COLOR_TEXT, 0);
    text_style_apply(s_text_lb);
    lv_obj_set_width(s_text_lb, PAGE_TEXT_W);
    lv_obj_set_pos(s_text_lb, RD_MARGIN, UI_TITLEBAR_H + 4);
    lv_label_set_long_mode(s_text_lb, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_text_lb, "");

    /* 测量用的 label：字体、行距、字距、宽度都要跟正文一致才量得准。
     * 挪到屏幕下方而不是用 HIDDEN —— 隐藏对象不参与布局计算。 */
    s_measure = lv_label_create(root);
    text_style_apply(s_measure);
    lv_obj_set_width(s_measure, PAGE_TEXT_W);
    lv_label_set_long_mode(s_measure, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(s_measure, 0, UI_SCREEN_H + 20);

    /* 底部：页码 + 进度 */
    s_info_lb = lv_label_create(root);
    lv_obj_set_style_text_color(s_info_lb, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_info_lb, &font_ui_14, 0);
    lv_label_set_text(s_info_lb, "");
    lv_obj_align(s_info_lb, LV_ALIGN_BOTTOM_MID, 0, -3);

    /* 自动翻页：每次进书都按「关闭」起步，间隔沿用上次设置 */
    s_auto_sec  = auto_sec_load();
    s_auto_on   = false;
    s_auto_hold = false;

    /* 自动翻页进度线：2px 细横线，开启后从正文顶部扫到底部，扫到底翻下一页。
     * 后创建 = 画在正文之上，不会被文字盖住。 */
    s_auto_line = lv_obj_create(root);
    lv_obj_remove_flag(s_auto_line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_auto_line, PAGE_TEXT_W, AUTO_LINE_H);
    lv_obj_set_pos(s_auto_line, RD_MARGIN, AUTO_LINE_TOP);
    lv_obj_set_style_bg_color(s_auto_line, UI_COLOR_PRIMARY, 0);
    lv_obj_set_style_bg_opa(s_auto_line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_auto_line, 0, 0);
    lv_obj_set_style_radius(s_auto_line, 0, 0);
    lv_obj_set_style_pad_all(s_auto_line, 0, 0);
    lv_obj_add_flag(s_auto_line, LV_OBJ_FLAG_HIDDEN);

    /* 翻页交互统一挂在 root 上：滑动按方向翻、轻点按落点翻（左右各三分之一）。
     * 不再单独建透明点击带 —— 那样「在带上滑动」会既走手势又走点击，翻两页。 */
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root, reader_pressed_cb,    LV_EVENT_PRESSED,    NULL);
    lv_obj_add_event_cb(root, reader_released_cb,   LV_EVENT_RELEASED,   NULL);
    lv_obj_add_event_cb(root, reader_press_lost_cb, LV_EVENT_PRESS_LOST, NULL);

    /* 长按正文 → 阅读菜单（查看封面 / 查看简介 / 跳转章节页码） */
    lv_obj_add_event_cb(root, on_reader_long, LV_EVENT_LONG_PRESSED, NULL);
}

/**
 * @brief 进正文：备好标题栏，建正文控件，再恢复上次的位置
 *
 * 标题栏在这里 ensure 一道，是防止「正文建好了却没有标题栏」——阅读器
 * 没有手势返回，退出全靠标题栏上那个返回键，漏建就等于把用户关在书里
 * 出不来（异步打开那条路就漏过一次）。正常流程里加载阶段已经建好了，
 * 这里只是兜底。
 */
static void reader_enter(lv_obj_t *root)
{
    reader_titlebar_ensure(root, srv_reader_title());
    reader_update_title();
    reader_build_content(root);
    reader_resume(root);
}

/**
 * @brief 恢复上次读到的位置，并重建页表
 */
static void reader_resume(lv_obj_t *root)
{
    srv_reader_load_progress();

    ESP_LOGI(TAG, "《%s》共 %d 字 %d 章，从字偏移 %ld 恢复",
             srv_reader_title(), srv_reader_char_count(),
             srv_reader_chapter_count(), (long)srv_reader_position());

    /* 页表得从书头一路推到目标位置（同步还是分片由 reader_start_layout 定）。
     * 只把「上次的偏移」当第一页的话，页码会重新从 1 开始、往回也翻不动。 */
    reader_start_layout(root, srv_reader_position());
}

void ui_reader_create(lv_obj_t *root)
{
    /* 清理回调必须无条件挂上：加载中也可能退出（例如点完书就按返回），
     * 而定时器不会随页面对象一起销毁 —— 不在入口挂，加载完成时
     * open_poll_cb 会拿着已经释放的 root / 等待层去操作，直接崩。 */
    lv_obj_add_event_cb(root, reader_root_del_cb, LV_EVENT_DELETE, NULL);

    if (!srv_reader_busy() && !srv_reader_is_open()) {
        ui_titlebar_create(root, "阅读", app_navigator_back_cb, NULL);
        lv_obj_t *lb = lv_label_create(root);
        lv_obj_set_style_text_color(lb, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_font(lb, &font_ui_14, 0);
        lv_label_set_text(lb, "没有打开的书");
        lv_obj_align(lb, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    /* 标题栏第一件事就建：加载期间也要在（唯一的退路是它上面的返回键），
     * 书名要等打开完才拿得到，到时候用 set_title 换字 */
    reader_titlebar_ensure(root, srv_reader_busy() ? "打开中" : srv_reader_title());

    /* 后台还在打开：铺等待层，剩下的交给 open_poll_cb。
     * 点完书立刻就有反应，SD 上的慢活不占着 LVGL 任务。 */
    if (srv_reader_busy()) {
        wait_layer_create(root, "正在打开", 0);
        wait_layer_note("正在读取书籍…");
        s_open_poll = lv_timer_create(open_poll_cb, 50, root);
        return;
    }

    reader_enter(root);
}
