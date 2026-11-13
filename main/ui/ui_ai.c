#include "ui_ai.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "app_navigator.h"
#include "srv_ai.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_ai";

#define TICK_MS     200
/*! 「按住说话」宽：横屏居中 200；竖屏收窄并靠左，给右侧音量键让位 */
#define BTN_W       ((ui_scr_w() >= 300) ? 200 : (ui_scr_w() - 80))
#define BTN_H       48

static lv_obj_t *s_state_lb = NULL;     /*!< 状态文本（正在连接… / 按住说话…） */
static lv_obj_t *s_code_lb  = NULL;     /*!< 激活码大字 */
static lv_obj_t *s_hint_lb  = NULL;     /*!< 激活提示 */
static lv_obj_t *s_text_lb  = NULL;     /*!< 最近一句对话（识别结果 / AI 回复） */
static lv_obj_t *s_btn      = NULL;
static lv_obj_t *s_btn_lb   = NULL;

static void talk_button_style(srv_ai_state_t st)
{
    const char *text = "按住说话";
    lv_color_t  color = UI_COLOR_PRIMARY;
    bool        muted = false;

    switch (st) {
    case SRV_AI_LISTENING:
        text  = "正在听…";
        color = UI_COLOR_TEMP;
        break;
    case SRV_AI_SPEAKING:
        /* 回答中可以按下去打断（设备没有回声消除，打断时先停喇叭再开麦） */
        text  = "按住打断";
        color = UI_COLOR_TEMP;
        break;
    case SRV_AI_READY:
        text  = "按住说话";
        break;
    case SRV_AI_CONNECTING:
        text  = "连接中…";
        muted = true;
        break;
    case SRV_AI_ACTIVATING:
        text  = "等待绑定…";
        muted = true;
        break;
    case SRV_AI_ERROR:
        text  = "重试";
        break;
    default:
        text  = "按住说话";
        muted = true;
        break;
    }

    lv_label_set_text(s_btn_lb, text);
    lv_obj_set_style_bg_color(s_btn, muted ? UI_COLOR_SURFACE : color, 0);
}

static void ai_refresh(void)
{
    if (s_state_lb == NULL) {
        return;
    }

    srv_ai_state_t st = srv_ai_state();

    lv_label_set_text(s_state_lb, srv_ai_state_text());

    /* 激活码：只在待绑定时亮出来 */
    const char *code = srv_ai_activation_code();
    if (code[0] != '\0') {
        lv_label_set_text(s_code_lb, code);
        lv_obj_clear_flag(s_code_lb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_hint_lb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_text_lb, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_code_lb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_hint_lb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_text_lb, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_text_lb, srv_ai_last_text());
    }

    talk_button_style(st);
}

static void ai_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    ai_refresh();
}

/* ---------------------- 按住说话 ---------------------- */

static void on_talk_pressed(lv_event_t *e)
{
    (void)e;
    srv_ai_state_t st = srv_ai_state();

    /* 出错状态下按一下 = 重连 */
    if (st == SRV_AI_ERROR || st == SRV_AI_IDLE) {
        esp_err_t ret = srv_ai_start();
        if (ret != ESP_OK) {
            lv_label_set_text(s_state_lb, "上一轮还没断开，稍等");
        }
        return;
    }

    (void)srv_ai_talk(true);
    ai_refresh();
}

static void on_vol_click(lv_event_t *e)
{
    (void)e;
    ui_volume_popup_show();
}

static void on_talk_released(lv_event_t *e)
{
    (void)e;
    (void)srv_ai_talk(false);
    ai_refresh();
}

static void ai_root_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    lv_timer_t *t = (lv_timer_t *)lv_event_get_user_data(e);
    if (t != NULL) {
        lv_timer_delete(t);
    }

    /* 退出页面 = 结束会话（后台会自己收尾，这里不阻塞） */
    srv_ai_stop();

    s_state_lb = NULL;
    s_code_lb  = NULL;
    s_hint_lb  = NULL;
    s_text_lb  = NULL;
    s_btn      = NULL;
    s_btn_lb   = NULL;
}

void ui_ai_create(lv_obj_t *root)
{
    ui_titlebar_create(root, "小智 AI", app_navigator_back_smart_cb, NULL);

    /* 服务端下发的激活码：6 位数字，用时钟大字显示最醒目 */
    s_code_lb = lv_label_create(root);
    lv_obj_set_style_text_font(s_code_lb, &font_clock_48, 0);
    lv_obj_set_style_text_color(s_code_lb, UI_COLOR_PRIMARY, 0);
    lv_label_set_text(s_code_lb, "");
    lv_obj_align(s_code_lb, LV_ALIGN_TOP_MID, 0, UI_TITLEBAR_H + 34);
    lv_obj_add_flag(s_code_lb, LV_OBJ_FLAG_HIDDEN);

    s_hint_lb = lv_label_create(root);
    lv_obj_set_style_text_font(s_hint_lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_align(s_hint_lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_hint_lb, "打开 xiaozhi.me 控制台\n添加设备并输入上面的激活码");
    lv_obj_set_width(s_hint_lb, ui_scr_w() - 24);
    lv_obj_align(s_hint_lb, LV_ALIGN_TOP_MID, 0, UI_TITLEBAR_H + 88);
    lv_obj_add_flag(s_hint_lb, LV_OBJ_FLAG_HIDDEN);

    /* 状态行 */
    s_state_lb = lv_label_create(root);
    lv_obj_set_style_text_font(s_state_lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_state_lb, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_align(s_state_lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_state_lb, ui_scr_w() - 16);
    lv_obj_set_height(s_state_lb, font_ui_14.line_height);
    lv_label_set_long_mode(s_state_lb, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(s_state_lb, 6000, 0);
    lv_label_set_text(s_state_lb, "正在连接…");
    lv_obj_align(s_state_lb, LV_ALIGN_TOP_MID, 0, UI_TITLEBAR_H + 6);

    /* 最近一句对话：激活阶段让位给激活码，激活后显示识别结果/AI 回复 */
    s_text_lb = lv_label_create(root);
    lv_obj_set_style_text_font(s_text_lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_text_lb, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_align(s_text_lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_text_lb, ui_scr_w() - 24);
    lv_obj_set_height(s_text_lb, 72);
    lv_label_set_long_mode(s_text_lb, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_text_lb, "");
    lv_obj_align(s_text_lb, LV_ALIGN_TOP_MID, 0, UI_TITLEBAR_H + 26);

    /* 按住说话 */
    s_btn = lv_button_create(root);
    lv_obj_set_size(s_btn, BTN_W, BTN_H);
    if (ui_scr_w() >= 300) {
        lv_obj_align(s_btn, LV_ALIGN_BOTTOM_MID, 0, -14);
    } else {
        /* 竖屏：靠左摆，右侧留给音量键 */
        lv_obj_align(s_btn, LV_ALIGN_BOTTOM_LEFT, 8, -14);
    }
    lv_obj_set_style_radius(s_btn, 10, 0);
    lv_obj_set_style_shadow_width(s_btn, 0, 0);
    lv_obj_set_style_pad_all(s_btn, 0, 0);
    lv_obj_set_style_bg_color(s_btn, UI_COLOR_PRIMARY, 0);
    ui_add_click_feedback(s_btn);
    lv_obj_add_event_cb(s_btn, on_talk_pressed,  LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(s_btn, on_talk_released, LV_EVENT_RELEASED, NULL);
    /* 手指滑出按钮时不会收到 RELEASED，必须靠 PRESS_LOST 收尾，
     * 否则麦克风会一直开着 */
    lv_obj_add_event_cb(s_btn, on_talk_released, LV_EVENT_PRESS_LOST, NULL);

    s_btn_lb = lv_label_create(s_btn);
    lv_obj_set_style_text_font(s_btn_lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_btn_lb, lv_color_white(), 0);
    lv_label_set_text(s_btn_lb, "按住说话");
    lv_obj_center(s_btn_lb);

    /* 右侧音量键：和「按住说话」同一行，弹通用的音量条 */
    lv_obj_t *vol = lv_button_create(root);
    lv_obj_set_size(vol, 48, BTN_H);
    lv_obj_set_pos(vol, ui_scr_w() - 8 - 48, ui_scr_h() - 14 - BTN_H);
    lv_obj_set_style_radius(vol, 10, 0);
    lv_obj_set_style_bg_color(vol, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_shadow_width(vol, 0, 0);
    lv_obj_set_style_pad_all(vol, 0, 0);
    ui_add_click_feedback(vol);
    lv_obj_add_event_cb(vol, on_vol_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *vic = lv_image_create(vol);
    lv_image_set_src(vic, &ic_volume);
    lv_obj_set_style_image_recolor(vic, UI_COLOR_TEXT, 0);
    lv_obj_set_style_radius(vic, 0, 0);
    lv_obj_center(vic);

    ai_refresh();

    /* 进页面就开会话；上一轮还没退干净时会返回错误，界面显示出来即可 */
    esp_err_t ret = srv_ai_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "启动会话失败: %s", esp_err_to_name(ret));
        lv_label_set_text(s_state_lb, "上一轮还没断开，稍等再试");
    }

    lv_timer_t *tmr = lv_timer_create(ai_tick_cb, TICK_MS, NULL);
    lv_obj_add_event_cb(root, ai_root_del_cb, LV_EVENT_DELETE, tmr);
}
