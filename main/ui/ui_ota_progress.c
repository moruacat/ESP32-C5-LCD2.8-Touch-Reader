#include "ui_ota_progress.h"
#include "ui_common.h"
#include "srv_ota.h"

#include "lvgl.h"

static lv_obj_t *s_layer;
static lv_obj_t *s_title;
static lv_obj_t *s_bar;
static lv_obj_t *s_pct;

static void tick_cb(lv_timer_t *timer)
{
    (void)timer;

    srv_ota_progress_t p;
    srv_ota_get_progress(&p);

    const bool visible = p.active || p.done;
    if (!visible) {
        if (!lv_obj_has_flag(s_layer, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if (lv_obj_has_flag(s_layer, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_layer);
    }

    if (p.done) {
        lv_label_set_text(s_title, "更新完成，正在重启 ...");
        lv_bar_set_value(s_bar, 100, LV_ANIM_OFF);
        lv_label_set_text(s_pct, "100%");
        return;
    }

    lv_label_set_text(s_title, "正在更新固件");

    int pct = p.percent;
    if (pct < 0 && p.total > 0) {
        pct = (int)((p.received * 100u) / p.total);
    }
    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }
    lv_bar_set_value(s_bar, pct, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_pct, "%d%%", pct);
}

void ui_ota_progress_init(void)
{
    if (s_layer != NULL) {
        return;
    }

    /* 全屏遮罩：更新期间别让用户点到下面的页面 */
    s_layer = lv_obj_create(lv_layer_top());
    lv_obj_remove_flag(s_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_layer, ui_scr_w(), ui_scr_h());
    lv_obj_set_pos(s_layer, 0, 0);
    lv_obj_set_style_bg_color(s_layer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_layer, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_layer, 0, 0);
    lv_obj_set_style_radius(s_layer, 0, 0);
    lv_obj_set_style_pad_all(s_layer, 0, 0);

    s_title = lv_label_create(s_layer);
    lv_obj_set_style_text_font(s_title, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_title, lv_color_white(), 0);
    lv_label_set_text(s_title, "正在更新固件");
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -34);

    s_bar = lv_bar_create(s_layer);
    lv_obj_set_size(s_bar, 220, 12);
    lv_obj_align(s_bar, LV_ALIGN_CENTER, 0, 0);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x30363D), LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, UI_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 6, LV_PART_INDICATOR);

    s_pct = lv_label_create(s_layer);
    lv_obj_set_style_text_font(s_pct, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_pct, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(s_pct, "0%");
    lv_obj_align(s_pct, LV_ALIGN_CENTER, 0, 30);

    lv_obj_add_flag(s_layer, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create(tick_cb, 200, NULL);
}
