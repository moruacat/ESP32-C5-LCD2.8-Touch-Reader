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

/* ===================== 长按菜单 ===================== */

/* 跳转弹层（章节 / 页码）在下面实现，这里先声明 */
static void goto_show(void);

enum {
    ACT_COVER = 0,
    ACT_INTRO,
    ACT_GOTO,
};

static int s_menu_act[UI_MENU_MAX_ITEMS];
static char s_book_name[SRV_BOOK_NAME_MAX];

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
        return;                         /* 取消，或点了菜单外的空白 */
    }

    switch (s_menu_act[idx]) {
    case ACT_COVER:
        act_show_cover();
        break;
    case ACT_INTRO:
        act_show_intro();
        break;
    case ACT_GOTO:
        goto_show();
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

static lv_obj_t *s_goto_mask;
static lv_obj_t *s_goto_input;          /*!< 顶部输入显示 */
static lv_obj_t *s_goto_roller;         /*!< EBF 的章节滚轮；TXT 为 NULL */
static char     *s_goto_opts;           /*!< 滚轮选项文本（PSRAM/堆，弹层关掉时释放） */
static char      s_goto_digits[GOTO_DIGITS_BUF];
static int       s_goto_digit_n;
static int       s_goto_input_max = 4;  /*!< 本次允许输入的位数 */

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
    s_goto_roller  = NULL;
    s_goto_digit_n = 0;
    s_goto_digits[0] = '\0';

    free(s_goto_opts);                  /* 滚轮自己存了一份，这里只是还缓冲 */
    s_goto_opts = NULL;
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

/* 输入框显示：没输入时显示当前定位（EBF 是滚轮选中项，TXT 是当前页） */
static void goto_input_refresh(void)
{
    if (s_goto_input == NULL) {
        return;
    }

    char buf[32];
    if (s_goto_digit_n > 0) {
        snprintf(buf, sizeof(buf), "第 %s %s", s_goto_digits,
                 (s_goto_roller != NULL) ? "章" : "页");
    } else if (s_goto_roller != NULL) {
        snprintf(buf, sizeof(buf), "第 %d 章",
                 (int)lv_roller_get_selected(s_goto_roller) + 1);
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
    if (s_goto_roller == NULL) {
        return;
    }

    int cnt = srv_reader_chapter_count();
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

    lv_roller_set_selected(s_goto_roller, (uint32_t)(v - 1), LV_ANIM_OFF);
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

    if (s_goto_roller != NULL) {
        /* 章节号已经在输入时定位到滚轮上了，这里就是确认进去 */
        goto_chapter_jump((int)lv_roller_get_selected(s_goto_roller));
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

/* EBF 的章节列表：用滚轮而不是逐章建对象 —— 上千章会建出几千个 LVGL 对象，
 * 内部 RAM 扛不住；滚轮的选项只是一个字符串，还能原生支持滚动定位 */
static bool goto_build_chapters(lv_obj_t *box)
{
    int cnt = srv_reader_chapter_count();
    if (cnt <= 0) {
        return false;
    }

    char title[CHAP_TITLE_MAX];

    /* 每项一行，内容是**真章名**（索引里查，纯内存）。两趟走：先量总长再分配，
     * 比"按每项预留 N 字节"准，也不会把列表尾巴截掉。 */
    size_t cap = 8;
    for (int i = 0; i < cnt; i++) {
        srv_reader_chapter_title(i, title, sizeof(title));
        cap += strlen(title) + 1;
    }

    /* 章数不设上限，几千章就是几十 KB，内部 RAM 不一定给得出 → 先试 PSRAM */
    s_goto_opts = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_goto_opts == NULL) {
        s_goto_opts = malloc(cap);
    }
    if (s_goto_opts == NULL) {
        return false;
    }

    size_t p = 0;
    for (int i = 0; i < cnt; i++) {
        srv_reader_chapter_title(i, title, sizeof(title));
        p += (size_t)snprintf(s_goto_opts + p, cap - p, "%s\n", title);
    }
    if (p > 0) {
        s_goto_opts[p - 1] = '\0';                  /* 去掉末尾的换行 */
    }

    lv_obj_t *r = lv_roller_create(box);
    lv_roller_set_options(r, s_goto_opts, LV_ROLLER_MODE_NORMAL);
    lv_obj_set_size(r, GOTO_LIST_W, GOTO_LIST_H);
    lv_obj_set_pos(r, 8, 36);
    lv_obj_set_style_bg_color(r, UI_COLOR_BG, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_set_style_text_font(r, &font_ui_14, 0);
    lv_obj_set_style_text_color(r, UI_COLOR_TEXT_DIM, 0);
    /* 行距要在算高度之前设好 */
    lv_obj_set_style_text_line_space(r, 6, 0);
    lv_roller_set_visible_row_count(r, 6);
    lv_obj_set_style_bg_color(r, UI_COLOR_PRIMARY, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, lv_color_white(), LV_PART_SELECTED);
    lv_obj_set_style_radius(r, 6, LV_PART_SELECTED);

    int cur = srv_reader_chapter_index();
    if (cur < 0 || cur >= cnt) {
        cur = 0;
    }
    lv_roller_set_selected(r, (uint32_t)cur, LV_ANIM_OFF);

    s_goto_roller = r;
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

    /* 翻页交互统一挂在 root 上：滑动按方向翻、轻点按落点翻（左右各三分之一）。
     * 不再单独建透明点击带 —— 那样「在带上滑动」会既走手势又走点击，翻两页。 */
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root, reader_pressed_cb,    LV_EVENT_PRESSED,    NULL);
    lv_obj_add_event_cb(root, reader_released_cb,   LV_EVENT_RELEASED,   NULL);
    lv_obj_add_event_cb(root, reader_press_lost_cb, LV_EVENT_PRESS_LOST, NULL);

    /* 长按正文 → 阅读菜单（查看封面 / 查看简介 / 跳转章节页码） */
    lv_obj_add_event_cb(root, on_reader_long, LV_EVENT_LONG_PRESSED, NULL);

    lv_obj_add_event_cb(root, reader_root_del_cb, LV_EVENT_DELETE, NULL);
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
