#include "ui_countdown.h"
#include "ui_common.h"
#include "ui_dialog.h"
#include "app_navigator.h"
#include "srv_timer.h"
#include "ui_sleep.h"
#include "bsp/esp32_c5_touch_lcd_2.8.h"

#include "esp_log.h"

#include <stdio.h>
#include <stdbool.h>

static const char *TAG = "ui_countdown";

/* ===================== 布局 ===================== */
#define SETUP_Y         30
/*! 滚轮宽度跟着屏幕：横屏 70，竖屏 240 时收窄到 66，三个滚轮不越界 */
#define ROLLER_W        ((ui_scr_w() >= 300) ? 70 : ((ui_scr_w() - 16 - ROLLER_GAP * 2) / 3))
#define ROLLER_GAP      12
#define ROLLER_TOP      6
#define ROLLER_LINE_SP  8       /*!< 滚轮行距：14px 字体太密看不清，拉开一点 */

/* 底部三个按钮：宽度和位置按屏幕算，竖屏时整体收窄、贴底 */
#define BTN_Y           (ui_scr_h() - 60)
#define BTN_H           48
#define BTN_X0          14
#define BTN_GAP         8
#define BTN_RESET_W     ((ui_scr_w() >= 300) ? 72 : 56)
#define BTN_SOUND_W     ((ui_scr_w() >= 300) ? 96 : 64)
#define BTN_RESET_X     BTN_X0
#define BTN_MAIN_X      (BTN_RESET_X + BTN_RESET_W + BTN_GAP)
#define BTN_MAIN_W      (ui_scr_w() - BTN_X0 * 2 - BTN_RESET_W - BTN_SOUND_W - BTN_GAP * 2)
#define BTN_SOUND_X     (BTN_MAIN_X + BTN_MAIN_W + BTN_GAP)

/* 标题栏右端最外层是旋转键（8 + 24）。横屏把 BOOT 启停放它左边；
 * 竖屏标题栏太窄，挪到左下（底部按钮上方），免得压住标题和滚轮。 */
#define BOOT_TAB_W      92
#define BOOT_TAB_H      20
#define BOOT_TAB_X      ((ui_scr_w() >= 300) ? (ui_scr_w() - BOOT_TAB_W - 40) : 8)
#define BOOT_TAB_Y      ((ui_scr_w() >= 300) ? 4 : (ui_scr_h() - BTN_H - 40))

#define TICK_MS         100
#define ALERT_CLOSE_MS  8000    /*!< 到点提示层自动消失时间 */

static lv_obj_t *s_setup_box;               /*!< 滚轮 + 单位标签，设定时显示 */
static lv_obj_t *s_roller_h, *s_roller_m, *s_roller_s;
static lv_obj_t *s_big_lb;                  /*!< 运行态剩余时间 */
static lv_obj_t *s_sub_lb;                  /*!< 运行态状态文字 */
static lv_obj_t *s_main_lb;                 /*!< 主按钮文字 */
static lv_obj_t *s_sound_lb;                /*!< 提示音按钮文字 */
static lv_obj_t *s_boot_btn;
static lv_obj_t *s_root;
static bool      s_boot_ctl = false;        /*!< 运行时开关：是否允许 BOOT 键启停倒计时 */

static bool s_setup_form = true;            /*!< 当前是「设定」还是「运行」形态 */

/* ===================== 工具 ===================== */

/* 秒 → "HH:MM:SS" */
static void fmt_hms(char *buf, size_t n, int64_t sec)
{
    if (sec < 0) {
        sec = 0;
    }
    snprintf(buf, n, "%02d:%02d:%02d",
             (int)(sec / 3600), (int)((sec / 60) % 60), (int)(sec % 60));
}

/* 统一样式的按钮；返回按钮上的文字标签。
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
    lv_obj_set_style_pad_all(btn, 0, 0);
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

/* ===================== 滚轮 ===================== */

static lv_obj_t *make_roller(lv_obj_t *parent, lv_coord_t x, int count, int def_sel)
{
    /* 选项文本 "0\n1\n...\n(count-1)"：最大 59 项，长度约 170 字节 */
    char   buf[200];
    size_t p = 0;

    for (int i = 0; i < count; i++) {
        if (i > 0) {
            buf[p++] = '\n';
        }
        p += (size_t)snprintf(buf + p, sizeof(buf) - p, "%d", i);
    }
    buf[p] = '\0';

    lv_obj_t *r = lv_roller_create(parent);
    lv_roller_set_options(r, buf, LV_ROLLER_MODE_INFINITE);
    lv_obj_set_width(r, ROLLER_W);
    lv_obj_set_pos(r, x, ROLLER_TOP);
    lv_obj_set_style_bg_color(r, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_set_style_text_font(r, &font_ui_14, 0);
    lv_obj_set_style_text_color(r, UI_COLOR_TEXT_DIM, 0);
    /* 行距要在算高度之前设好，否则高度是按默认行距定的 */
    lv_obj_set_style_text_line_space(r, ROLLER_LINE_SP, 0);
    lv_roller_set_visible_row_count(r, 3);

    /* 选中行 */
    lv_obj_set_style_bg_color(r, UI_COLOR_PRIMARY, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, lv_color_white(), LV_PART_SELECTED);
    lv_obj_set_style_radius(r, 6, LV_PART_SELECTED);

    lv_roller_set_selected(r, (uint32_t)def_sel, LV_ANIM_OFF);
    return r;
}

/* ===================== 到点提示 ===================== */

/* 用通用提示层（挂在活动屏幕上，用户在哪一页都能看到）。
 * 通用实现带「先删面板再回调」和自动关闭，这里不需要额外东西。 */
static void countdown_done_cb(void *arg)
{
    (void)arg;

    if (bsp_display_lock(UI_LOCK_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "取 LVGL 锁失败，跳过到点提示");
        return;
    }
    ui_alert_show("倒计时结束", UI_COLOR_WARN, NULL, "知道了",
                  NULL, NULL, ALERT_CLOSE_MS);
    bsp_display_unlock();
}

/* ===================== 页面逻辑 ===================== */

static void cd_sound_refresh(void)
{
    bool on = srv_timer_sound_enabled();
    lv_label_set_text(s_sound_lb, on ? "提示音 开" : "提示音 关");
    lv_obj_set_style_text_color(s_sound_lb, on ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM, 0);
}

/* 「设定时长」和「运行中」两套控件的显隐 */
static void cd_apply_form(bool setup)
{
    s_setup_form = setup;

    /* 大字只认「时间」，设定形态下由滚轮承担显示 */
    if (setup) {
        lv_obj_remove_flag(s_setup_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_big_lb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_sub_lb, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_setup_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_big_lb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_sub_lb, LV_OBJ_FLAG_HIDDEN);
    }
}

static void cd_refresh(void)
{
    bool finished = srv_timer_cd_is_finished();
    bool running  = srv_timer_cd_is_running();
    bool setup    = (!srv_timer_cd_is_active() && !finished);

    if (setup != s_setup_form) {
        cd_apply_form(setup);
    }
    if (setup) {
        /* 回到设定形态时按钮也要复位成「开始」，
         * 否则重置后按钮还留着「暂停/继续」，看着像没清掉 */
        lv_label_set_text(s_main_lb, "开始");
        return;
    }

    /* 剩余时间向上取整：刚设好 5 分钟时应当显示 00:05:00 而不是 00:04:59 */
    char buf[24];
    fmt_hms(buf, sizeof(buf), (srv_timer_cd_remaining_ms() + 999) / 1000);
    lv_label_set_text(s_big_lb, buf);

    if (finished) {
        lv_obj_set_style_text_color(s_big_lb, UI_COLOR_TEMP, 0);
        lv_label_set_text(s_sub_lb, "时间到");
        lv_label_set_text(s_main_lb, "重新开始");
    } else {
        lv_obj_set_style_text_color(s_big_lb, UI_COLOR_TEXT, 0);
        lv_label_set_text(s_sub_lb, running ? "运行中" : "已暂停");
        lv_label_set_text(s_main_lb, running ? "暂停" : "继续");
    }
}

static void cd_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    cd_refresh();
}

static void on_main(lv_event_t *e)
{
    (void)e;

    /* 现在是不是「设定」形态要用实时状态判断，别依赖 100ms 才同步一次的 s_setup_form */
    if (!srv_timer_cd_is_active() && !srv_timer_cd_is_finished()) {
        srv_timer_cd_set_hms((int)lv_roller_get_selected(s_roller_h),
                             (int)lv_roller_get_selected(s_roller_m),
                             (int)lv_roller_get_selected(s_roller_s));
    }

    srv_timer_cd_toggle();
    cd_refresh();
}

static void on_reset(lv_event_t *e)
{
    (void)e;
    srv_timer_cd_reset();
    cd_refresh();
}

static void on_sound(lv_event_t *e)
{
    (void)e;
    srv_timer_set_sound(!srv_timer_sound_enabled());
    cd_sound_refresh();
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
    ui_sleep_set_force_touch_wake(s_boot_ctl, s_root);
}

static lv_obj_t *make_boot_tab(lv_obj_t *root)
{
    lv_obj_t *btn = lv_button_create(root);
    lv_obj_set_size(btn, BOOT_TAB_W, BOOT_TAB_H);
    lv_obj_set_pos(btn, BOOT_TAB_X, BOOT_TAB_Y);
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

bool ui_countdown_boot_control_enabled(void)
{
    return s_root != NULL && s_boot_ctl;
}

void ui_countdown_boot_toggle(void)
{
    if (!ui_countdown_boot_control_enabled()) {
        return;
    }
    if (bsp_display_lock(UI_LOCK_TIMEOUT_MS) != ESP_OK) {
        return;
    }

    if (!srv_timer_cd_is_active() && !srv_timer_cd_is_finished()) {
        srv_timer_cd_set_hms((int)lv_roller_get_selected(s_roller_h),
                             (int)lv_roller_get_selected(s_roller_m),
                             (int)lv_roller_get_selected(s_roller_s));
    }
    srv_timer_cd_toggle();
    cd_refresh();
    bsp_display_unlock();
}

static void cd_root_del_cb(lv_event_t *e)
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

void ui_countdown_create(lv_obj_t *root)
{
    s_root = root;
    ui_titlebar_create_plain(root, "倒计时", app_navigator_back_cb, NULL);
    s_boot_btn = make_boot_tab(root);
    boot_tab_refresh();
    ui_sleep_set_force_touch_wake(s_boot_ctl, s_root);

    /* 到点提示由本页负责：谁启动倒计时谁就得把提示层准备好。
     * 回调是文件内静态函数，页面销毁后指针仍然有效，切到别的页面也能弹。 */
    srv_timer_register_done_cb(countdown_done_cb, NULL);

    /* ===== 设定形态：三列滚轮 ===== */
    s_setup_box = lv_obj_create(root);
    lv_obj_remove_flag(s_setup_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_setup_box, ui_scr_w(), SETUP_Y + 120);
    lv_obj_set_pos(s_setup_box, 0, SETUP_Y);
    lv_obj_set_style_bg_opa(s_setup_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_setup_box, 0, 0);
    lv_obj_set_style_radius(s_setup_box, 0, 0);
    lv_obj_set_style_pad_all(s_setup_box, 0, 0);

    /* 滚轮初值取当前设定的时长，从别的页面返回时不会丢失 */
    int64_t total_s = srv_timer_cd_total_ms() / 1000;
    int     h0 = (int)(total_s / 3600);
    int     m0 = (int)((total_s / 60) % 60);
    int     s0 = (int)(total_s % 60);

    const lv_coord_t x0 = (ui_scr_w() - (ROLLER_W * 3 + ROLLER_GAP * 2)) / 2;
    s_roller_h = make_roller(s_setup_box, x0, 24, h0);
    s_roller_m = make_roller(s_setup_box, x0 + ROLLER_W + ROLLER_GAP, 60, m0);
    s_roller_s = make_roller(s_setup_box, x0 + (ROLLER_W + ROLLER_GAP) * 2, 60, s0);

    const char       *units[3] = {"时", "分", "秒"};
    const lv_obj_t   *rollers[3] = {s_roller_h, s_roller_m, s_roller_s};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *lb = lv_label_create(s_setup_box);
        lv_obj_set_style_text_color(lb, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_font(lb, &font_ui_14, 0);
        lv_label_set_text(lb, units[i]);
        lv_obj_align_to(lb, rollers[i], LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    }

    /* ===== 运行形态：大字剩余时间 + 状态 ===== */
    s_big_lb = lv_label_create(root);
    lv_obj_set_style_text_color(s_big_lb, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_big_lb, &font_clock_48, 0);
    lv_label_set_text(s_big_lb, "00:00:00");
    lv_obj_align(s_big_lb, LV_ALIGN_CENTER, 0, -18);
    lv_obj_add_flag(s_big_lb, LV_OBJ_FLAG_HIDDEN);

    s_sub_lb = lv_label_create(root);
    lv_obj_set_style_text_color(s_sub_lb, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_sub_lb, &font_ui_14, 0);
    lv_label_set_text(s_sub_lb, "运行中");
    lv_obj_align(s_sub_lb, LV_ALIGN_CENTER, 0, 26);
    lv_obj_add_flag(s_sub_lb, LV_OBJ_FLAG_HIDDEN);

    /* ===== 底部按钮 ===== */
    s_main_lb = make_btn(root, BTN_MAIN_X, BTN_MAIN_W, true, "开始", on_main);
    make_btn(root, BTN_RESET_X, BTN_RESET_W, false, "重置", on_reset);
    s_sound_lb = make_btn(root, BTN_SOUND_X, BTN_SOUND_W, false, "提示音 开", on_sound);

    cd_sound_refresh();

    s_setup_form = true;
    cd_refresh();                       /* 按真实状态定形态（例如倒计时还在跑） */

    lv_timer_t *tmr = lv_timer_create(cd_tick_cb, TICK_MS, NULL);
    lv_obj_add_event_cb(root, cd_root_del_cb, LV_EVENT_DELETE, tmr);
}
