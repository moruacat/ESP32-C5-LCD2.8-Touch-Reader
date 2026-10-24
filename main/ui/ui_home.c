#include "ui_home.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "ui_dialog.h"
#include "app_navigator.h"
#include "srv_config.h"
#include "srv_safe.h"
#include "bsp/esp32_c5_touch_lcd_2.8.h"

#include <stdio.h>
#include <stdlib.h>

#define CARD_W      96
#define CARD_H      160
#define CARD_Y      32
#define CARD_X0     8
#define CARD_GAP    8

static lv_obj_t *s_root;
static lv_obj_t *s_bright_val;      /*!< 亮度数值文字 */
static lv_obj_t *s_bright_slider;   /*!< 亮度条（点数值精确调的时候要同步它） */

/* ===================== 亮度 ===================== */

/* 统一的亮度设置：写配置 + 立刻改背光 + 同步亮度条与数值 */
static void bright_apply(int v)
{
    if (v < UI_BRIGHTNESS_MIN) {
        v = UI_BRIGHTNESS_MIN;
    }
    if (v > UI_BRIGHTNESS_MAX) {
        v = UI_BRIGHTNESS_MAX;
    }

    bsp_display_brightness_set(v);
    srv_config_set_int(CFG_BRIGHTNESS, v, true);

    if (s_bright_slider != NULL && lv_obj_is_valid(s_bright_slider)) {
        lv_slider_set_value(s_bright_slider, v, LV_ANIM_OFF);
    }
    if (s_bright_val != NULL && lv_obj_is_valid(s_bright_val)) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", v);
        lv_label_set_text(s_bright_val, buf);
    }
}

/* 拖亮度条：实时改背光 + 显示数值，落盘交给 deferred 合并 */
static void on_brightness(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(obj);

    bsp_display_brightness_set(v);
    srv_config_set_int(CFG_BRIGHTNESS, v, false);
    srv_config_save_deferred();

    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", v);
    lv_label_set_text(s_bright_val, buf);
}

static void on_bright_pick_done(const int *v, void *arg)
{
    (void)arg;
    bright_apply(v[0]);
}

/* 点数值：弹一个 1~100 的滚轮，精确选到想要的档位 */
static void on_bright_val_click(lv_event_t *e)
{
    (void)e;

    const ui_picker_col_t cols[1] = {
        {"亮度 %", UI_BRIGHTNESS_MIN, UI_BRIGHTNESS_MAX - UI_BRIGHTNESS_MIN + 1,
         (int)srv_config()->brightness},
    };
    ui_picker_show("亮度", 1, cols, on_bright_pick_done, NULL);
}

/* ===================== 卡片回调 ===================== */

static void on_func(lv_event_t *e)
{
    (void)e;
    app_navigator_switch(PAGE_FUNC, true);
}

static void on_settings(lv_event_t *e)
{
    (void)e;
    app_navigator_switch(PAGE_SETTINGS, true);
}

static void on_timer(lv_event_t *e)
{
    (void)e;
    app_navigator_switch(PAGE_TIMER, true);
}

static void on_countdown(lv_event_t *e)
{
    (void)e;
    app_navigator_switch(PAGE_COUNTDOWN, true);
}

static void on_words(lv_event_t *e)
{
    (void)e;
    app_navigator_switch(PAGE_WORDS, true);
}

/*! 录音机（安全模式下顶替「功能」卡的位置） */
static void on_recorder(lv_event_t *e)
{
    (void)e;
    app_navigator_switch(PAGE_RECORDER, true);
}

/* 底部返回键：回时钟页 */
static void on_back(lv_event_t *e)
{
    (void)e;
    app_navigator_hide();
}

/* 页面销毁时清掉 static 指针（比对目标，避免清掉新页面的） */
static void home_root_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    if (lv_event_get_target(e) != s_root) {
        return;
    }
    s_root          = NULL;
    s_bright_val    = NULL;
    s_bright_slider = NULL;
}

void ui_home_create(lv_obj_t *root)
{
    s_root = root;

    /* 主菜单的返回入口放在底部亮度条左侧，标题栏不再重复放返回键 */
    ui_titlebar_create(root, "主菜单", NULL, NULL);

    /* 左：功能。
     * 安全模式下整张卡不出现（书架 / 音乐 / 游戏 / 照片 / 录音机都在里面），
     * 空出来的位置改摆一张「录音机」卡 —— 录音机是个再普通不过的功能，
     * 正好把那列填满，不显得"少了一块"。 */
    if (!srv_safe_on()) {
        ui_card_create(root, CARD_X0, CARD_Y, CARD_W, CARD_H,
                       &ic_func, "功能", 0x1585C0, on_func, NULL);
    } else {
        ui_card_create(root, CARD_X0, CARD_Y, CARD_W, CARD_H,
                       &ic_record, "录音机", 0xE5484D, on_recorder, NULL);
    }

    /* 中：计时器 / 倒计时 上下分割，共用一个卡片空间 */
    lv_coord_t mid_x = CARD_X0 + CARD_W + CARD_GAP;
    lv_coord_t half_h = (CARD_H - CARD_GAP) / 2;
    ui_card_create(root, mid_x, CARD_Y, CARD_W, half_h,
                   &ic_timer, "计时器", 0xF5A623, on_timer, NULL);
    ui_card_create(root, mid_x, CARD_Y + half_h + CARD_GAP, CARD_W, half_h,
                   &ic_countdown, "倒计时", 0xE5484D, on_countdown, NULL);

    /* 右：单词 / 设置 上下分割，共用一个卡片空间（单词在上、设置在下） */
    lv_coord_t right_x = CARD_X0 + (CARD_W + CARD_GAP) * 2;
    ui_card_create(root, right_x, CARD_Y, CARD_W, half_h,
                   &ic_words, "单词", 0x3B9EFF, on_words, NULL);
    ui_card_create(root, right_x, CARD_Y + half_h + CARD_GAP, CARD_W, half_h,
                   &ic_setting, "设置", 0x606A78, on_settings, NULL);

    /* ===== 底部：返回键 + 亮度图标 + 数值 + 亮度条 ===== */
    lv_obj_t *back = lv_button_create(root);
    lv_obj_set_size(back, 26, 24);
    lv_obj_align(back, LV_ALIGN_BOTTOM_LEFT, 8, -10);
    lv_obj_set_style_radius(back, 6, 0);
    lv_obj_set_style_bg_color(back, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    ui_add_click_feedback(back);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *ic = lv_image_create(back);
        lv_image_set_src(ic, &ic_arrow_left);
        lv_obj_set_style_image_recolor(ic, UI_COLOR_TEXT, 0);
        lv_obj_set_style_radius(ic, 0, 0);
        lv_obj_center(ic);
    }

    lv_obj_t *bright_ic = lv_image_create(root);
    lv_image_set_src(bright_ic, &ic_brightness);
    lv_obj_set_style_image_recolor(bright_ic, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_radius(bright_ic, 0, 0);
    /* 图标原图 32x32，缩到 20 才不会顶到右边的百分比数字 */
    lv_image_set_scale(bright_ic, 20 * 256 / 32);
    lv_obj_align(bright_ic, LV_ALIGN_BOTTOM_LEFT, 42, -9);

    /* 亮度数值：贴着图标右边排。做成按钮，点它可以弹滚轮精确选档位 */
    lv_obj_t *val_btn = lv_button_create(root);
    lv_obj_set_size(val_btn, 48, 24);
    lv_obj_align_to(val_btn, bright_ic, LV_ALIGN_OUT_RIGHT_MID, 4, 0);
    lv_obj_set_style_radius(val_btn, 6, 0);
    lv_obj_set_style_bg_opa(val_btn, LV_OPA_TRANSP, 0);   /* 平时只要文字，不要底色 */
    lv_obj_set_style_shadow_width(val_btn, 0, 0);
    ui_add_click_feedback(val_btn);
    lv_obj_add_event_cb(val_btn, on_bright_val_click, LV_EVENT_CLICKED, NULL);

    s_bright_val = lv_label_create(val_btn);
    lv_obj_set_style_text_color(s_bright_val, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_bright_val, &font_ui_14, 0);
    lv_obj_center(s_bright_val);

    /* 亮度条：下限 1%，0% 留给休眠，不允许在界面上调到 0
     *
     * 滑块直径 = slider 高度(18)，而它是以「轨道端点为圆心」画的，滑到
     * 100% 时右半边会露到 slider 外面，slider 又贴着屏幕右边缘，于是被切掉。
     *
     * 这里不用「给轨道加 padding」来收滑块：padding 会让轨道的背景跟着
     * 内缩，而指示器（蓝条）是从内缩后的位置起画，两者之间会多出一段
     * 空隙，看起来灰色和蓝色没对齐。改成直接把 slider 缩短并右移，
     * 给滑块让出空间 —— 轨道本身不缩，蓝条从轨道左端起画，天然对齐。 */
    lv_obj_t *slider = lv_slider_create(root);
    lv_obj_remove_flag(slider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(slider, 162, 18);
    lv_obj_align(slider, LV_ALIGN_BOTTOM_RIGHT, -18, -13);
    lv_obj_set_style_bg_color(slider, UI_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, UI_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, UI_COLOR_PRIMARY, LV_PART_KNOB);
    lv_slider_set_range(slider, UI_BRIGHTNESS_MIN, UI_BRIGHTNESS_MAX);

    /* 用配置里的亮度，而不是实际背光：自动变暗会把背光压低，
     * 亮度条不该跟着掉下去（否则一进主菜单看到的就是变暗后的值） */
    int cur = (int)srv_config()->brightness;
    if (cur < UI_BRIGHTNESS_MIN) {
        cur = UI_BRIGHTNESS_MIN;
    }
    if (cur > UI_BRIGHTNESS_MAX) {
        cur = UI_BRIGHTNESS_MAX;
    }
    lv_slider_set_value(slider, cur, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, on_brightness, LV_EVENT_VALUE_CHANGED, NULL);

    s_bright_slider = slider;

    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", cur);
    lv_label_set_text(s_bright_val, buf);

    lv_obj_add_event_cb(root, home_root_del_cb, LV_EVENT_DELETE, NULL);
}