#include "ui_rotation_test.h"
#include "ui_common.h"
#include "ui_dialog.h"
#include "app_navigator.h"
#include "bsp/esp32_c5_touch_lcd_2.8.h"

#include "esp_log.h"

#include <stdint.h>
#include <stdio.h>

static const char *TAG = "ui_rot_test";

/* 自动退出是安全兜底：如果 MADCTL/触摸映射组合反了，用户仍然能在 20 秒后
 * 自动恢复横屏，不会把设备锁在摸不到退出按钮的方向。 */
#define ROT_TEST_AUTO_CLOSE_MS  20000

static lv_obj_t *s_mask;
static lv_obj_t *s_hint;
static lv_obj_t *s_btn_row;
static lv_obj_t *s_btn[4];
static lv_timer_t *s_timer;

static int current_angle(void)
{
    return (int)bsp_display_get_rotation();
}

static void test_timer_delete(void)
{
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
}

static void test_mask_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    if (lv_event_get_target(e) != s_mask) {
        return;
    }
    s_mask = NULL;
    s_hint = NULL;
    s_btn_row = NULL;
    for (int i = 0; i < 4; i++) {
        s_btn[i] = NULL;
    }
    test_timer_delete();
}

static void test_close(void)
{
    if (s_mask == NULL && s_timer == NULL) {
        return;
    }

    /* 先回到默认横屏，再销毁测试层；最后退回设置页，让原页面按横屏重建。 */
    (void)bsp_display_set_rotation(0);

    lv_obj_t *mask = s_mask;
    s_mask = NULL;
    test_timer_delete();
    if (mask != NULL && lv_obj_is_valid(mask)) {
        lv_obj_delete(mask);
    }

    app_navigator_back();
}

static void test_timeout_cb(lv_timer_t *timer)
{
    (void)timer;
    ESP_LOGW(TAG, "旋转测试超时，自动恢复横屏");
    test_close();
}

static void reset_timeout(void)
{
    if (s_timer != NULL) {
        lv_timer_reset(s_timer);
    }
}

static void apply_button_style(lv_obj_t *btn, bool active)
{
    if (btn == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(btn, active ? UI_COLOR_PRIMARY : UI_COLOR_SURFACE, 0);
    lv_obj_t *lb = lv_obj_get_child(btn, 0);
    if (lb != NULL) {
        lv_obj_set_style_text_color(lb, active ? lv_color_white() : UI_COLOR_TEXT, 0);
    }
}

static void test_refresh_layout(void)
{
    if (s_mask == NULL || !lv_obj_is_valid(s_mask)) {
        return;
    }

    int w = (int)lv_display_get_horizontal_resolution(NULL);
    int h = (int)lv_display_get_vertical_resolution(NULL);
    int angle = current_angle();
    if (w <= 0 || h <= 0) {
        return;
    }

    lv_obj_set_size(s_mask, w, h);
    lv_obj_set_pos(s_mask, 0, 0);
    lv_obj_set_size(s_btn_row, w - 16, 36);
    lv_obj_align(s_btn_row, LV_ALIGN_BOTTOM_MID, 0, -10);

    char buf[96];
    snprintf(buf, sizeof(buf), "旋转测试\n当前 %d°\n逻辑分辨率 %d × %d\n下方按钮应可正常点击",
             angle, w, h);
    lv_label_set_text(s_hint, buf);

    for (int i = 0; i < 4; i++) {
        int deg = i * 90;
        apply_button_style(s_btn[i], angle == deg);
    }

    lv_obj_update_layout(s_mask);
}

static void on_angle_clicked(lv_event_t *e)
{
    int deg = (int)(intptr_t)lv_event_get_user_data(e);
    esp_err_t ret = bsp_display_set_rotation((uint16_t)deg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "设置旋转 %d° 失败: %s", deg, esp_err_to_name(ret));
        return;
    }
    test_refresh_layout();
    reset_timeout();
}

static void on_close_clicked(lv_event_t *e)
{
    (void)e;
    test_close();
}

static lv_obj_t *make_test_btn(lv_obj_t *parent, lv_coord_t w, const char *text,
                               lv_event_cb_t cb, void *arg)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, 30);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    ui_add_click_feedback(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, arg);

    lv_obj_t *lb = lv_label_create(btn);
    lv_obj_set_style_text_font(lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(lb, UI_COLOR_TEXT, 0);
    lv_label_set_text(lb, text);
    lv_obj_center(lb);
    return btn;
}

static lv_obj_t *make_corner(lv_obj_t *parent, const char *text, lv_align_t align,
                             lv_coord_t x, lv_coord_t y, lv_color_t color)
{
    lv_obj_t *lb = lv_label_create(parent);
    lv_obj_set_style_text_font(lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(lb, color, 0);
    lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lb, text);
    lv_obj_align(lb, align, x, y);
    return lb;
}

void ui_rotation_test_show(void)
{
    if (s_mask != NULL && lv_obj_is_valid(s_mask)) {
        lv_obj_move_foreground(s_mask);
        return;
    }

    ui_alert_close();

    s_mask = lv_obj_create(lv_layer_top());
    lv_obj_remove_flag(s_mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_mask, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_mask, UI_COLOR_PRIMARY, 0);
    lv_obj_set_style_border_width(s_mask, 2, 0);
    lv_obj_set_style_radius(s_mask, 0, 0);
    lv_obj_set_style_pad_all(s_mask, 0, 0);
    lv_obj_add_event_cb(s_mask, test_mask_del_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *title = lv_label_create(s_mask);
    lv_obj_set_style_text_font(title, &font_ui_14, 0);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    lv_label_set_text(title, "旋转测试 · 20 秒自动退出");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    make_corner(s_mask, "左上\nTL", LV_ALIGN_TOP_LEFT, 8, 32, UI_COLOR_TEXT);
    make_corner(s_mask, "右上\nTR", LV_ALIGN_TOP_RIGHT, -8, 32, UI_COLOR_TEMP);
    make_corner(s_mask, "左下\nBL", LV_ALIGN_BOTTOM_LEFT, 8, -52, UI_COLOR_HUMI);
    make_corner(s_mask, "右下\nBR", LV_ALIGN_BOTTOM_RIGHT, -8, -52, UI_COLOR_OK);

    s_hint = lv_label_create(s_mask);
    lv_obj_set_style_text_font(s_hint, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_hint, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, -12);

    s_btn_row = lv_obj_create(s_mask);
    lv_obj_remove_flag(s_btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_btn_row, 0, 0);
    lv_obj_set_style_radius(s_btn_row, 0, 0);
    lv_obj_set_style_pad_all(s_btn_row, 0, 0);
    lv_obj_set_flex_flow(s_btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_btn_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    static const char *const labels[4] = {"0°", "90°", "180°", "270°"};
    for (int i = 0; i < 4; i++) {
        s_btn[i] = make_test_btn(s_btn_row, 42, labels[i], on_angle_clicked,
                                 (void *)(intptr_t)(i * 90));
    }
    make_test_btn(s_btn_row, 52, "退出", on_close_clicked, NULL);

    s_timer = lv_timer_create(test_timeout_cb, ROT_TEST_AUTO_CLOSE_MS, NULL);
    test_refresh_layout();
}
