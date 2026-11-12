#include "ui_timer.h"
#include "ui_common.h"
#include "app_navigator.h"
#include "srv_timer.h"
#include "ui_sleep.h"
#include "bsp/esp32_c5_touch_lcd_2.8.h"

#include <stdio.h>

/* 底部两个按钮：左「重置」，右「开始/暂停/继续」 */
#define BTN_Y           180
#define BTN_H           48
#define BTN_RESET_X     8
#define BTN_RESET_W     96
#define BTN_MAIN_X      112
#define BTN_MAIN_W      200

/* 标题栏右侧的 BOOT 启停开关（风格同单词页的查词/译按钮） */
#define BOOT_TAB_W      92
#define BOOT_TAB_H      20
#define BOOT_TAB_X      (UI_SCREEN_W - BOOT_TAB_W - 8)

/*! 刷新周期：百分秒显示，50ms 一跳（20Hz）。
 *  .hh 那两位本来就是每 10ms 才变一次，20Hz 时每次跳 5 个数，看着一样是连续跑动；
 *  但 48px 大字每帧都要重新排版 + 重绘，单核 C5 上省下的重绘量是实打实的。 */
#define TICK_MS         50

static lv_obj_t *s_time_lb;
static lv_obj_t *s_main_lb;
static lv_obj_t *s_boot_btn;
static lv_obj_t *s_root;
static bool      s_boot_ctl = false;   /*!< 运行时开关：是否允许 BOOT 键启停秒表 */

static int64_t s_last_ms     = -1;
static bool    s_last_active = false;

/* 毫秒 → "MM:SS.hh"；分钟不封顶，超过 99 分钟自然变成三位数 */
static void fmt_stopwatch(char *buf, size_t n, int64_t ms)
{
    if (ms < 0) {
        ms = 0;
    }
    int64_t cs = ms / 10;
    int     m  = (int)(cs / 6000);
    int     s  = (int)((cs / 100) % 60);
    int     hh = (int)(cs % 100);
    snprintf(buf, n, "%02d:%02d.%02d", m, s, hh);
}

static void sw_update(void)
{
    bool    running = srv_timer_sw_is_running();
    bool    active  = srv_timer_sw_is_active();
    int64_t ms      = srv_timer_sw_elapsed_ms();

    /* 停着且数值没变就不写 label：暂停/归零后不必每 20ms 重绘一次 */
    if (!running && ms == s_last_ms && active == s_last_active) {
        return;
    }

    char buf[24];
    fmt_stopwatch(buf, sizeof(buf), ms);
    lv_label_set_text(s_time_lb, buf);
    lv_label_set_text(s_main_lb, running ? "暂停" : (active ? "继续" : "开始"));

    s_last_ms     = ms;
    s_last_active = active;
}

static void sw_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    sw_update();
}

static void on_toggle(lv_event_t *e)
{
    (void)e;
    srv_timer_sw_toggle();
    sw_update();
}

static void on_reset(lv_event_t *e)
{
    (void)e;
    srv_timer_sw_reset();
    sw_update();
}

/* 统一按钮外观；返回按钮上的文字标签供后续改字。
 * 文字必须在这里就设一次：lv_label 的默认文本是 "Text"，
 * 不设的话按钮上直接显示 "Text"。 */
static lv_obj_t *make_btn(lv_obj_t *root, lv_coord_t x, lv_coord_t w,
                          bool primary, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(root);
    lv_obj_set_size(btn, w, BTN_H);
    lv_obj_set_pos(btn, x, BTN_Y);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, primary ? UI_COLOR_PRIMARY : UI_COLOR_SURFACE, 0);
    ui_add_click_feedback(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lb = lv_label_create(btn);
    lv_obj_set_style_text_color(lb, primary ? lv_color_white() : UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(lb, &font_ui_14, 0);
    lv_label_set_text(lb, text);
    lv_obj_center(lb);

    return lb;
}

/* ===================== BOOT 键启停开关 ===================== */

static void boot_tab_refresh(void)
{
    if (s_boot_btn == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(s_boot_btn,
                              s_boot_ctl ? UI_COLOR_PRIMARY : UI_COLOR_SURFACE, 0);
    lv_obj_t *lb = lv_obj_get_child(s_boot_btn, 0);
    if (lb != NULL) {
        lv_obj_set_style_text_color(lb,
                                    s_boot_ctl ? lv_color_white() : UI_COLOR_TEXT_DIM, 0);
        lv_label_set_text(lb, s_boot_ctl ? "BOOT启停 开" : "BOOT启停 关");
    }
}

static void on_boot_ctl_toggle(lv_event_t *e)
{
    (void)e;
    s_boot_ctl = !s_boot_ctl;
    boot_tab_refresh();
    /* 强制触摸唤醒只改运行时覆盖，不写 CFG_TOUCH_WAKE。 */
    ui_sleep_set_force_touch_wake(s_boot_ctl, s_root);
}

static lv_obj_t *make_boot_tab(lv_obj_t *root)
{
    lv_obj_t *btn = lv_button_create(root);
    lv_obj_set_size(btn, BOOT_TAB_W, BOOT_TAB_H);
    lv_obj_set_pos(btn, BOOT_TAB_X, 4);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    ui_add_click_feedback(btn);
    lv_obj_add_event_cb(btn, on_boot_ctl_toggle, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lb = lv_label_create(btn);
    lv_obj_set_style_text_font(lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(lb, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(lb, "BOOT启停 关");
    lv_obj_center(lb);
    return btn;
}

bool ui_timer_boot_control_enabled(void)
{
    return s_root != NULL && s_boot_ctl;
}

void ui_timer_boot_toggle(void)
{
    if (!ui_timer_boot_control_enabled()) {
        return;
    }
    if (bsp_display_lock(UI_LOCK_TIMEOUT_MS) != ESP_OK) {
        return;
    }
    srv_timer_sw_toggle();
    sw_update();
    bsp_display_unlock();
}

/* 页面销毁时删掉刷新定时器（同 ui_clock：句柄走事件 user_data，
 * 不能用 static 存，否则新页面先建好时会被覆盖） */
static void timer_root_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    lv_timer_t *t = (lv_timer_t *)lv_event_get_user_data(e);
    if (t != NULL) {
        lv_timer_delete(t);
    }

    if (lv_event_get_target(e) == s_root) {
        ui_sleep_set_force_touch_wake(false, s_root);
        s_root = NULL;
        s_boot_btn = NULL;
    }
}

void ui_timer_create(lv_obj_t *root)
{
    s_root = root;
    ui_titlebar_create_plain(root, "计时器", app_navigator_back_cb, NULL);
    s_boot_btn = make_boot_tab(root);
    boot_tab_refresh();
    ui_sleep_set_force_touch_wake(s_boot_ctl, s_root);

    s_time_lb = lv_label_create(root);
    lv_obj_set_style_text_color(s_time_lb, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_time_lb, &font_clock_48, 0);
    lv_label_set_text(s_time_lb, "00:00.00");
    lv_obj_align(s_time_lb, LV_ALIGN_CENTER, 0, -22);

    s_main_lb = make_btn(root, BTN_MAIN_X, BTN_MAIN_W, true, "开始", on_toggle);
    make_btn(root, BTN_RESET_X, BTN_RESET_W, false, "重置", on_reset);

    /* 页面每次重建都会重置这两个记录值，强制立刻刷一次真实时间 */
    s_last_ms     = -1;
    s_last_active = false;
    sw_update();

    lv_timer_t *tmr = lv_timer_create(sw_tick_cb, TICK_MS, NULL);
    lv_obj_add_event_cb(root, timer_root_del_cb, LV_EVENT_DELETE, tmr);
}
