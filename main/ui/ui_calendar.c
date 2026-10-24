/**
 * @file ui_calendar.c
 * @brief 日历：月历视图（时钟页上滑进来）
 *
 * 布局（横屏 320x240）：
 *   标题栏 / 「‹ 2026年10月 ›」+「今天」 / 星期表头 / 6 行日期格子
 *
 * 日期全部来自 RTC（srv_rtc），当天用主题色打底。翻月只改 s_year/s_month，
 * 格子整块重建 —— 一个月最多 42 个格子，重建比逐个改文字省心得多。
 */
#include "ui_calendar.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "app_navigator.h"

#include "srv_rtc.h"
#include "esp_log.h"

#include <stdio.h>

static const char *TAG = "ui_calendar";

#define CELL_W      44
#define CELL_H      24
#define GRID_X      ((UI_SCREEN_W - CELL_W * 7) / 2)    /* 6：7 列居中 */
#define GRID_Y      70
#define HEAD_Y      54

static const char *const WEEK_HDR[7] = { "日", "一", "二", "三", "四", "五", "六" };

static lv_obj_t *s_root;        /*!< 本页根容器（销毁回调里比对用） */
static lv_obj_t *s_title;       /*!< 「2026年10月」 */
static lv_obj_t *s_grid;        /*!< 日期格子容器 */
static int       s_year;
static int       s_month;

/* ===================== 日期计算 ===================== */

static bool leap_year(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int days_in_month(int y, int m)
{
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m < 1 || m > 12) {
        return 30;
    }
    if (m == 2 && leap_year(y)) {
        return 29;
    }
    return d[m - 1];
}

/*! 翻月：delta = ±1 */
static void month_shift(int delta)
{
    s_month += delta;
    if (s_month < 1) {
        s_month = 12;
        s_year--;
    } else if (s_month > 12) {
        s_month = 1;
        s_year++;
    }
}

/* ===================== 重建 ===================== */

static void cal_rebuild(void)
{
    if (s_grid == NULL || !lv_obj_is_valid(s_grid)) {
        return;
    }
    lv_obj_clean(s_grid);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d年%d月", s_year, s_month);
    lv_label_set_text(s_title, buf);

    srv_rtc_time_t now;
    const bool have_now = (srv_rtc_get_time(&now) == ESP_OK);
    const bool this_month = have_now && (now.year == s_year) && (now.month == s_month);

    /* 1 号是星期几（0 = 周日），后面按列排 */
    const int first = srv_rtc_calc_dotw(s_year, s_month, 1);
    const int days  = days_in_month(s_year, s_month);

    for (int d = 1; d <= days; d++) {
        const int idx = first + d - 1;
        const int col = idx % 7;
        const int row = idx / 7;

        lv_obj_t *cell = lv_obj_create(s_grid);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        /* 格子不需要被点：留着可点会把触摸吃掉，root 就收不到滑动事件了 */
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(cell, CELL_W - 2, CELL_H - 2);
        lv_obj_set_pos(cell, col * CELL_W, row * CELL_H);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_radius(cell, 4, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);

        const bool today = this_month && (now.day == d);
        if (today) {
            lv_obj_set_style_bg_color(cell, UI_COLOR_PRIMARY, 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        }

        lv_obj_t *lb = lv_label_create(cell);
        lv_obj_set_style_text_font(lb, &font_ui_14, 0);
        lv_obj_set_style_text_color(lb, today ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM, 0);
        snprintf(buf, sizeof(buf), "%d", d);
        lv_label_set_text(lb, buf);
        lv_obj_center(lb);
    }
}

/* ===================== 交互 ===================== */

static void on_prev(lv_event_t *e)
{
    (void)e;
    month_shift(-1);
    cal_rebuild();
}

static void on_next(lv_event_t *e)
{
    (void)e;
    month_shift(+1);
    cal_rebuild();
}

static void on_today(lv_event_t *e)
{
    (void)e;

    srv_rtc_time_t t;
    if (srv_rtc_get_time(&t) == ESP_OK) {
        s_year  = t.year;
        s_month = t.month;
    }
    cal_rebuild();
}

/*! 翻月小按钮：圆角深底 + 箭头图标 */
static lv_obj_t *nav_btn(lv_obj_t *parent, lv_coord_t x, const lv_image_dsc_t *icon,
                         lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 30, 26);
    lv_obj_set_pos(btn, x, 30);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    ui_add_click_feedback(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ic = lv_image_create(btn);
    lv_image_set_src(ic, icon);
    lv_obj_set_style_image_recolor(ic, UI_COLOR_TEXT, 0);
    lv_obj_set_style_radius(ic, 0, 0);
    lv_obj_center(ic);
    return btn;
}

static void cal_root_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    /* 手快「上滑又返回」时新页面可能已经建好，比对目标再清 */
    if (lv_event_get_target(e) != s_root) {
        return;
    }
    s_root  = NULL;
    s_title = NULL;
    s_grid  = NULL;
}

void ui_calendar_create(lv_obj_t *root)
{
    s_root = root;

    ui_titlebar_create(root, "日历", app_navigator_back_cb, NULL);

    /* 从当前月份开始（RTC 读不到就给个不会崩的默认值） */
    srv_rtc_time_t now;
    if (srv_rtc_get_time(&now) == ESP_OK) {
        s_year  = now.year;
        s_month = now.month;
    } else {
        s_year  = 2026;
        s_month = 1;
    }

    /* ---- 翻月行：‹ 年月 › ---- */
    (void)nav_btn(root, 6, &ic_arrow_left, on_prev);
    (void)nav_btn(root, UI_SCREEN_W - 36, &ic_arrow_right, on_next);

    s_title = lv_label_create(root);
    lv_obj_set_style_text_color(s_title, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_title, &font_ui_14, 0);
    lv_label_set_text(s_title, "");
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 36);

    /* ---- 星期表头 ---- */
    for (int i = 0; i < 7; i++) {
        lv_obj_t *h = lv_label_create(root);
        lv_obj_set_style_text_color(h, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_font(h, &font_ui_14, 0);
        lv_obj_set_width(h, CELL_W);
        lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(h, WEEK_HDR[i]);
        lv_obj_set_pos(h, GRID_X + i * CELL_W, HEAD_Y);
    }

    /* ---- 日期格子（6 行最多 42 格，装得下一个月的所有排法） ---- */
    s_grid = lv_obj_create(root);
    lv_obj_remove_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_grid, LV_OBJ_FLAG_CLICKABLE);   /* 同上：别挡右滑返回 */
    lv_obj_set_size(s_grid, CELL_W * 7, CELL_H * 6);
    lv_obj_set_pos(s_grid, GRID_X, GRID_Y);
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_grid, 0, 0);
    lv_obj_set_style_radius(s_grid, 0, 0);
    lv_obj_set_style_pad_all(s_grid, 0, 0);

    /* ---- 底部：今天 + 回到本月 ---- */
    lv_obj_t *today = lv_label_create(root);
    lv_obj_set_style_text_color(today, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(today, &font_ui_14, 0);
    if (srv_rtc_get_time(&now) == ESP_OK) {
        char buf[32];
        snprintf(buf, sizeof(buf), "今天 %04d-%02d-%02d", now.year, now.month, now.day);
        lv_label_set_text(today, buf);
    } else {
        lv_label_set_text(today, "RTC 未就绪");
    }
    lv_obj_set_pos(today, 8, UI_SCREEN_H - 26);

    lv_obj_t *btn = lv_button_create(root);
    lv_obj_set_size(btn, 56, 22);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_RIGHT, -8, -4);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    ui_add_click_feedback(btn);
    lv_obj_add_event_cb(btn, on_today, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *lb = lv_label_create(btn);
        lv_obj_set_style_text_color(lb, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(lb, &font_ui_14, 0);
        lv_label_set_text(lb, "今天");
        lv_obj_center(lb);
    }

    lv_obj_add_event_cb(root, cal_root_del_cb, LV_EVENT_DELETE, NULL);

    cal_rebuild();
    ESP_LOGI(TAG, "日历就绪：%d年%d月", s_year, s_month);
}