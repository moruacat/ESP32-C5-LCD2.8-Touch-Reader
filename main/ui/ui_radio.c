#include "ui_radio.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "app_navigator.h"
#include "srv_radio.h"
#include "srv_audio.h"

#include <stdio.h>

/* ===================== 布局（跟随当前屏幕方向） ===================== */

#define NAME_Y      38
#define STATUS_Y    60

#define BTN_Y       96
#define BTN_H       48
#define BTN_GAP     8
#define X0          8
/*! 两个按钮等分一行：横屏 148，竖屏 108 */
#define BTN_W       ((ui_scr_w() - X0 * 2 - BTN_GAP) / 2)

#define VOL_Y       176
#define VOL_SLIDER_X 44
#define VOL_SLIDER_W (ui_scr_w() - VOL_SLIDER_X - 70)

#define TICK_MS     200

static lv_obj_t *s_root   = NULL;
static lv_obj_t *s_status = NULL;
static lv_obj_t *s_btn    = NULL;    /*!< 开始播放 / 暂停 / 继续 */
static lv_obj_t *s_btn_lb = NULL;
static lv_obj_t *s_stop   = NULL;
static lv_obj_t *s_vol    = NULL;
static lv_obj_t *s_vol_lb = NULL;

/* ===================== 控件 ===================== */

static lv_obj_t *make_btn(lv_obj_t *root, lv_coord_t x, uint32_t color,
                          const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(root);
    lv_obj_set_size(btn, BTN_W, BTN_H);
    lv_obj_set_pos(btn, x, BTN_Y);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    ui_add_click_feedback(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lb = lv_label_create(btn);
    lv_obj_set_style_text_font(lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(lb, lv_color_white(), 0);
    lv_label_set_text(lb, text);
    lv_obj_center(lb);
    return btn;
}

/* ===================== 状态刷新 ===================== */

static void radio_refresh(void)
{
    srv_radio_state_t st = srv_radio_state();

    switch (st) {
    case SRV_RADIO_PLAYING:
    case SRV_RADIO_BUFFERING:
        lv_label_set_text(s_btn_lb, "暂停");
        break;

    case SRV_RADIO_PAUSED:
        lv_label_set_text(s_btn_lb, "继续");
        break;

    default:                        /* SRV_RADIO_IDLE */
        lv_label_set_text(s_btn_lb, "开始播放");
        break;
    }

    lv_label_set_text(s_status, srv_radio_status_text());

    /* 停止键在空闲时没必要亮着 */
    lv_obj_set_style_bg_color(s_stop, (st == SRV_RADIO_IDLE) ? UI_COLOR_SURFACE
                                                            : UI_COLOR_TEMP, 0);

    int vol = srv_audio_get_volume();
    if (!lv_slider_is_dragged(s_vol)) {
        lv_slider_set_value(s_vol, vol, LV_ANIM_OFF);
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", vol);
    lv_label_set_text(s_vol_lb, buf);
}

static void radio_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    radio_refresh();
}

/* ===================== 按键 ===================== */

static void on_play(lv_event_t *e)
{
    (void)e;

    switch (srv_radio_state()) {
    case SRV_RADIO_BUFFERING:
    case SRV_RADIO_PLAYING:
        (void)srv_radio_pause();
        break;
    case SRV_RADIO_PAUSED:
        (void)srv_radio_resume();
        break;
    default:
        (void)srv_radio_start();
        break;
    }
    radio_refresh();                /* 先给个即时反馈，200ms 后的 tick 再校正 */
}

static void on_stop(lv_event_t *e)
{
    (void)e;
    (void)srv_radio_stop();
    radio_refresh();
}

static void on_vol(lv_event_t *e)
{
    int v = (int)lv_slider_get_value(lv_event_get_target(e));
    (void)srv_audio_set_volume(v);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", v);
    lv_label_set_text(s_vol_lb, buf);
}

/* ===================== 页面 ===================== */

static void radio_root_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    /* 切页是异步删除：手快「电台→别的→电台」时新页面已经建好，
     * 只有目标就是当前这份根对象时才清空 */
    if (lv_event_get_target(e) != s_root) {
        return;
    }

    lv_timer_t *t = (lv_timer_t *)lv_event_get_user_data(e);
    if (t != NULL) {
        lv_timer_delete(t);
    }

    /* 退页就停掉直播，别在后台占着扬声器 */
    if (srv_radio_state() != SRV_RADIO_IDLE) {
        (void)srv_radio_stop();
    }

    s_root   = NULL;
    s_status = NULL;
    s_btn    = NULL;
    s_btn_lb = NULL;
    s_stop   = NULL;
    s_vol    = NULL;
    s_vol_lb = NULL;
}

void ui_radio_create(lv_obj_t *root)
{
    s_root = root;

    ui_titlebar_create(root, "电台", app_navigator_back_smart_cb, NULL);

    /* 电台名 */
    lv_obj_t *name = lv_label_create(root);
    lv_obj_set_style_text_font(name, &font_ui_14, 0);
    lv_obj_set_style_text_color(name, UI_COLOR_TEXT, 0);
    lv_label_set_text(name, SRV_RADIO_NAME);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, NAME_Y);

    /* 状态行（正在连接… / 正在播放 · 96 kbps / 网络未连接） */
    s_status = lv_label_create(root);
    lv_obj_set_style_text_font(s_status, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_status, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_width(s_status, ui_scr_w() - 16);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_status, srv_radio_status_text());
    lv_obj_set_pos(s_status, X0, STATUS_Y);

    /* 播放/暂停 + 停止 */
    s_btn = make_btn(root, X0, 0x1585C0, "开始播放", on_play);
    s_btn_lb = lv_obj_get_child(s_btn, 0);

    s_stop = make_btn(root, X0 + BTN_W + BTN_GAP, 0xE5484D, "停止", on_stop);

    /* 音量 */
    lv_obj_t *ic = lv_image_create(root);
    lv_image_set_src(ic, &ic_volume);
    lv_obj_set_style_image_recolor(ic, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_radius(ic, 0, 0);
    lv_obj_align(ic, LV_ALIGN_TOP_LEFT, 12, VOL_Y - 2);

    s_vol = lv_slider_create(root);
    lv_obj_set_size(s_vol, VOL_SLIDER_W, 10);
    lv_obj_set_pos(s_vol, VOL_SLIDER_X, VOL_Y + 8);
    lv_slider_set_range(s_vol, 0, 100);
    lv_slider_set_value(s_vol, srv_audio_get_volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_vol, UI_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_vol, UI_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_vol, UI_COLOR_PRIMARY, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_vol, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(s_vol, on_vol, LV_EVENT_VALUE_CHANGED, NULL);

    s_vol_lb = lv_label_create(root);
    lv_obj_set_style_text_font(s_vol_lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_vol_lb, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(s_vol_lb, "0%");
    lv_obj_align(s_vol_lb, LV_ALIGN_TOP_RIGHT, -X0, VOL_Y);

    radio_refresh();

    lv_timer_t *tmr = lv_timer_create(radio_tick_cb, TICK_MS, NULL);
    lv_obj_add_event_cb(root, radio_root_del_cb, LV_EVENT_DELETE, tmr);
}