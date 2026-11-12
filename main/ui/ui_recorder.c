#include "ui_recorder.h"
#include "ui_common.h"
#include "app_navigator.h"
#include "srv_recorder.h"
#include "srv_sd.h"

#include <stdio.h>
#include <string.h>

#define BTN_W       200
#define BTN_H       48
#define TICK_MS     200

static lv_obj_t *s_time_lb;     /*!< 录音时长 */
static lv_obj_t *s_state_lb;    /*!< 状态/提示 */
static lv_obj_t *s_btn;
static lv_obj_t *s_btn_lb;

static void rec_update(void)
{
    bool    on  = srv_recorder_is_active();
    int64_t sec = srv_recorder_elapsed_ms() / 1000;

    /* 单文件上限一小时，连续录音会超过 60 分钟，超过就换成 H:MM:SS */
    char buf[16];
    if (sec >= 3600) {
        snprintf(buf, sizeof(buf), "%d:%02d:%02d",
                 (int)(sec / 3600), (int)((sec / 60) % 60), (int)(sec % 60));
    } else {
        snprintf(buf, sizeof(buf), "%02d:%02d", (int)(sec / 60), (int)(sec % 60));
    }
    lv_label_set_text(s_time_lb, buf);

    if (on) {
        lv_label_set_text(s_btn_lb, "停止录音");
        lv_obj_set_style_bg_color(s_btn, UI_COLOR_TEMP, 0);
        lv_label_set_text(s_state_lb, "录音中…");
    } else {
        lv_label_set_text(s_btn_lb, "开始录音");
        lv_obj_set_style_bg_color(s_btn, UI_COLOR_PRIMARY, 0);
    }
}

static void rec_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    rec_update();
}

static void on_btn(lv_event_t *e)
{
    (void)e;

    if (srv_recorder_is_active()) {
        srv_recorder_stop();
        rec_update();

        /* 停完显示存到哪儿了（只取文件名，完整路径太长） */
        const char *path = srv_recorder_last_file();
        const char *name = strrchr(path, '/');
        char buf[80];
        snprintf(buf, sizeof(buf), "已保存 /record/%s", name ? name + 1 : path);
        lv_label_set_text(s_state_lb, buf);
        return;
    }

    esp_err_t ret = srv_recorder_start();
    if (ret != ESP_OK) {
        lv_label_set_text(s_state_lb,
                          srv_sd_is_mounted() ? "启动失败" : "SD 卡未挂载");
        return;
    }
    rec_update();
}

static void rec_root_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    lv_timer_t *t = (lv_timer_t *)lv_event_get_user_data(e);
    if (t != NULL) {
        lv_timer_delete(t);
    }
}

void ui_recorder_create(lv_obj_t *root)
{
    ui_titlebar_create(root, "录音机", app_navigator_back_smart_cb, NULL);

    s_time_lb = lv_label_create(root);
    lv_obj_set_style_text_color(s_time_lb, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_time_lb, &font_clock_48, 0);
    lv_label_set_text(s_time_lb, "00:00");
    lv_obj_align(s_time_lb, LV_ALIGN_CENTER, 0, -30);

    s_state_lb = lv_label_create(root);
    lv_obj_set_style_text_color(s_state_lb, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_state_lb, &font_ui_14, 0);
    /* 存的路径可能很长（rec-123.wav），限宽 + 单行滚动，别撑出屏幕 */
    lv_obj_set_width(s_state_lb, UI_SCREEN_W - 16);
    lv_obj_set_height(s_state_lb, font_ui_14.line_height);
    lv_obj_set_style_text_align(s_state_lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_state_lb, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(s_state_lb, 6000, 0);
    lv_label_set_text(s_state_lb,
                      srv_sd_is_mounted() ? "文件保存到 /record" : "SD 卡未挂载");
    lv_obj_align(s_state_lb, LV_ALIGN_CENTER, 0, 22);

    s_btn = lv_button_create(root);
    lv_obj_set_size(s_btn, BTN_W, BTN_H);
    lv_obj_align(s_btn, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_radius(s_btn, 10, 0);
    lv_obj_set_style_shadow_width(s_btn, 0, 0);
    lv_obj_set_style_pad_all(s_btn, 0, 0);
    lv_obj_set_style_bg_color(s_btn, UI_COLOR_PRIMARY, 0);
    ui_add_click_feedback(s_btn);
    lv_obj_add_event_cb(s_btn, on_btn, LV_EVENT_CLICKED, NULL);

    s_btn_lb = lv_label_create(s_btn);
    lv_obj_set_style_text_font(s_btn_lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_btn_lb, lv_color_white(), 0);
    lv_label_set_text(s_btn_lb, "开始录音");
    lv_obj_center(s_btn_lb);

    rec_update();

    lv_timer_t *tmr = lv_timer_create(rec_tick_cb, TICK_MS, NULL);
    lv_obj_add_event_cb(root, rec_root_del_cb, LV_EVENT_DELETE, tmr);
}
