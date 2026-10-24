#include "ui_common.h"
#include "ui_icons.h"
#include "ui_dialog.h"
#include "app_navigator.h"
#include "srv_rtc.h"
#include "srv_safe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_theme_t *s_theme = NULL;

void ui_common_init(void)
{
    /* LVGL v9 默认主题即可满足深色需求，这里设置全局默认主题 */
    lv_theme_t *th = lv_theme_default_init(
        lv_display_get_default(),
        UI_COLOR_PRIMARY,          /* 主色 */
        UI_COLOR_SURFACE,          /* 次要色 */
        true,                      /* dark 主题 */
        &font_ui_14                /* 默认字体：中文 UI 字体 */
    );
    if (th) {
        lv_display_set_theme(lv_display_get_default(), th);
        s_theme = th;
    }
}

/* ===================== 页面骨架 ===================== */

lv_obj_t *ui_page_root_create(lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    /* 显式像素尺寸：不要用 LV_PCT，父对象是 screen，百分比解析不可靠 */
    lv_obj_set_size(root, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);      /* 必须不透明，否则旧界面会透出来 */
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    return root;
}

static void slide_anim_cb(void *var, int32_t v)
{
    lv_obj_set_style_translate_x((lv_obj_t *)var, (lv_coord_t)v, 0);
}

/*! 纵向滑入（日历从下往上）：同一个动画套路，改的只是 translate_y */
static void slide_v_anim_cb(void *var, int32_t v)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, (lv_coord_t)v, 0);
}

/**
 * @brief 滑入动画收尾时，把整屏再标脏一次
 *
 * 滑入过程中每一帧只重画 root 当前盖住的那部分；万一最后一帧没盖满
 * （动画被打断、或者首帧还没画就切走了），没盖到的区域会留着上一页的
 * 像素 —— 看起来就是"旧界面残影，怎么切都在"。
 * root 是 320x240 铺满整屏，invalidate 它就等于 invalidate 整屏。
 */
static void slide_done_cb(lv_anim_t *a)
{
    lv_obj_invalidate((lv_obj_t *)a->var);
}

void ui_slide_in(lv_obj_t *obj, bool from_right)
{
    ui_slide_in_dir(obj, from_right ? UI_SLIDE_FROM_RIGHT : UI_SLIDE_FROM_LEFT);
}

void ui_slide_in_dir(lv_obj_t *obj, ui_slide_dir_t dir)
{
    if (obj == NULL) {
        return;
    }

    const bool vertical = (dir == UI_SLIDE_FROM_BOTTOM);
    const lv_coord_t start = vertical ? UI_SCREEN_H
                                      : ((dir == UI_SLIDE_FROM_RIGHT) ? UI_SCREEN_W : -UI_SCREEN_W);

    if (vertical) {
        lv_obj_set_style_translate_y(obj, start, 0);
    } else {
        lv_obj_set_style_translate_x(obj, start, 0);
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, vertical ? slide_v_anim_cb : slide_anim_cb);
    lv_anim_set_values(&a, start, 0);
    lv_anim_set_duration(&a, 160);
    /* 缓动曲线用 ease_in_out：起步慢、中段快、收尾再慢下来。
     * 之前用的 ease_out 是一上来就冲到最快再减速，滑入距离只有一屏宽、
     * 时长又短，看着几乎和匀速一样。 */
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&a, slide_done_cb);
    lv_anim_start(&a);
}

/* ===================== 手势 ===================== */

static lv_point_t s_swipe_start;
static bool       s_swipe_tracking = false;
static lv_obj_t  *s_swipe_from     = NULL;

/* 起手对象是否落在滑块里（含祖先链） */
static bool is_slider_like(lv_obj_t *obj)
{
    while (obj != NULL) {
        if (lv_obj_check_type(obj, &lv_slider_class)) {
            return true;
        }
        obj = lv_obj_get_parent(obj);
    }
    return false;
}

static void swipe_pressed_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }
    lv_indev_get_point(indev, &s_swipe_start);
    s_swipe_from     = lv_event_get_target(e);
    s_swipe_tracking = true;
}

static void swipe_released_cb(lv_event_t *e)
{
    (void)e;
    if (!s_swipe_tracking) {
        return;
    }
    s_swipe_tracking = false;

    /* 拖滑块不算手势，否则调亮度会误触返回 */
    if (is_slider_like(s_swipe_from)) {
        return;
    }

    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    int dx = p.x - s_swipe_start.x;
    int dy = p.y - s_swipe_start.y;

    /* 上滑（只有时钟页）：打开日历，从下往上飞入。
     * 放在横向判断之前 —— 竖滑时 dx 很小，先被下面那行 return 挡掉了。 */
    if (dy <= -UI_SWIPE_MIN_PX && abs(dy) > abs(dx) * 2) {
        if (app_navigator_current() == PAGE_CLOCK) {
            app_navigator_switch_anim(PAGE_CALENDAR, true, APP_NAV_ANIM_BOTTOM);
        }
        return;
    }

    if (abs(dx) < UI_SWIPE_MIN_PX) {
        return;
    }
    if (abs(dx) < abs(dy) * 2) {
        return;                     /* 以纵向为主，不当作横向滑动 */
    }

    if (dx < 0) {
        /* 左滑：只有时钟页有下级页面 */
        if (app_navigator_current() == PAGE_CLOCK) {
            app_navigator_switch(PAGE_HOME, true);
        }
    } else {
        /* 右滑：时钟页上是安全模式手势的第二步（左上角连点 5 次 → 2 秒内右滑）
         * 打开安全菜单（左功能 / 右完整版设置），不是直接切安全模式 */
        if (app_navigator_current() == PAGE_CLOCK) {
            if (srv_safe_armed()) {
                srv_safe_disarm();
                app_navigator_switch_anim(PAGE_SAFE_MENU, true, APP_NAV_ANIM_LEFT);
            }
            return;                 /* 时钟页右滑本来就没有别的动作 */
        }
        app_navigator_back();
    }
}

static void swipe_lost_cb(lv_event_t *e)
{
    (void)e;
    s_swipe_tracking = false;
}

void ui_enable_swipe(lv_obj_t *root)
{
    if (root == NULL) {
        return;
    }
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root, swipe_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(root, swipe_released_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(root, swipe_lost_cb, LV_EVENT_PRESS_LOST, NULL);
}

/* ===================== 通用样式 ===================== */

void ui_add_click_feedback(lv_obj_t *obj)
{
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x243040), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_STATE_PRESSED);
}

/* ===================== 标题栏 ===================== */

static void titlebar_fill_time(lv_obj_t *label)
{
    srv_rtc_time_t t;
    if (srv_rtc_get_time(&t) != ESP_OK) {
        lv_label_set_text(label, "--:--");
        return;
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", t.hour, t.min);
    lv_label_set_text(label, buf);
}

static void titlebar_time_cb(lv_timer_t *timer)
{
    lv_obj_t *label = (lv_obj_t *)lv_timer_get_user_data(timer);
    titlebar_fill_time(label);
}

/**
 * @brief 标签随页面销毁时删掉定时器
 *
 * 只靠 lv_obj_is_valid() 判断是不够的：对象内存被复用后，
 * 那个指针可能"恰好"又指向一个有效对象，于是往错误的标签写文字。
 * 定时器句柄由事件 user_data 传入，避免 static 被新页面覆盖。
 */
static void titlebar_label_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    lv_timer_t *t = (lv_timer_t *)lv_event_get_user_data(e);
    if (t != NULL) {
        lv_timer_delete(t);
    }
}

/**
 * @brief 标题栏骨架
 * @param with_clock 右侧显示 HH:MM（普通页）；false 时右侧空出来给调用方放按钮（游戏页）
 */
static lv_obj_t *titlebar_build(lv_obj_t *parent, const char *title,
                                lv_event_cb_t back_cb, void *back_arg,
                                bool with_clock)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bar, UI_SCREEN_W, UI_TITLEBAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);

    /* 左侧返回键 */
    if (back_cb != NULL) {
        lv_obj_t *btn = lv_button_create(bar);
        lv_obj_set_size(btn, 26, 22);
        lv_obj_align(btn, LV_ALIGN_LEFT_MID, 2, 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_bg_color(btn, UI_COLOR_SURFACE, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        ui_add_click_feedback(btn);
        /* 两种事件都挂上：普通返回回调只认 CLICKED，智能返回还要用长按 */
        lv_obj_add_event_cb(btn, back_cb, LV_EVENT_CLICKED, back_arg);
        lv_obj_add_event_cb(btn, back_cb, LV_EVENT_LONG_PRESSED, back_arg);

        lv_obj_t *ic = lv_image_create(btn);
        lv_image_set_src(ic, &ic_arrow_left);
        lv_obj_set_style_image_recolor(ic, UI_COLOR_TEXT, 0);
        lv_obj_set_style_radius(ic, 0, 0);
        lv_obj_center(ic);
    }

    /* 标题居中。必须限宽 + 限一行高：书名/章名可能很长，不限的话 label 会
     * 按文字撑开、溢出标题栏被裁，而它是居中摆放的，看到的是**中间一截**。
     * 限了之后超出的部分按 LV_LABEL_LONG_DOT 收成省略号。 */
    lv_obj_t *ttl = lv_label_create(bar);
    lv_obj_set_style_text_color(ttl, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(ttl, &font_ui_14, 0);
    lv_label_set_text(ttl, title ? title : "");
    lv_label_set_long_mode(ttl, LV_LABEL_LONG_DOT);
    /* 文字必须在限宽之后仍居中：label 宽度是写死的（为了不被长标题顶出去），
     * 默认左对齐的话短标题会紧贴左边，看着整体偏左。 */
    lv_obj_set_style_text_align(ttl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(ttl, UI_SCREEN_W - (with_clock ? 112 : 84));   /* 让开两侧按钮/时钟 */
    lv_obj_set_height(ttl, (lv_coord_t)lv_font_get_line_height(&font_ui_14) + 2);
    lv_obj_align(ttl, LV_ALIGN_CENTER, 0, 0);
    /* 标题文字塞在 bar 的 user_data 里，ui_titlebar_set_title() 靠它改字。
     * 比按子对象下标去猜稳（有没有返回键会让下标错位）。 */
    lv_obj_set_user_data(bar, ttl);

    if (!with_clock) {
        return bar;                    /* 右侧那块留给调用方（游戏页放菜单键） */
    }

    /* 右侧 HH:MM */
    lv_obj_t *tm = lv_label_create(bar);
    lv_obj_set_style_text_color(tm, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(tm, &font_ui_14, 0);
    lv_label_set_text(tm, "--:--");
    lv_obj_align(tm, LV_ALIGN_RIGHT_MID, -6, 0);
    titlebar_fill_time(tm);                       /* 立即填一次 */
    /* 1 秒刷一次。
     * 原来写的是 20 秒，但标题栏只显示到分钟 —— 也就是最多要等 80 秒
     * 才可能看到数字变一下，看着就跟"时间不走"一样。
     * 读一次 RTC 就是一次 I2C 事务，开销可以忽略；而且文本没变化时
     * lv_label_set_text 内部会直接返回，不会产生多余的重绘。 */
    lv_timer_t *tmr = lv_timer_create(titlebar_time_cb, 1000, tm);
    lv_obj_add_event_cb(tm, titlebar_label_del_cb, LV_EVENT_DELETE, tmr);

    return bar;
}

lv_obj_t *ui_titlebar_create(lv_obj_t *parent, const char *title,
                             lv_event_cb_t back_cb, void *back_arg)
{
    return titlebar_build(parent, title, back_cb, back_arg, true);
}

lv_obj_t *ui_titlebar_create_plain(lv_obj_t *parent, const char *title,
                                   lv_event_cb_t back_cb, void *back_arg)
{
    return titlebar_build(parent, title, back_cb, back_arg, false);
}

void ui_titlebar_set_title(lv_obj_t *bar, const char *title)
{
    if (bar == NULL) {
        return;
    }

    lv_obj_t *ttl = (lv_obj_t *)lv_obj_get_user_data(bar);
    if (ttl == NULL) {
        return;
    }
    /* 文字没变时 lv_label_set_text 内部会直接返回，不会产生多余重绘 */
    lv_label_set_text(ttl, title ? title : "");
    lv_obj_align(ttl, LV_ALIGN_CENTER, 0, 0);   /* 换字后重新居中 */
}

/* ===================== 卡片 ===================== */

lv_obj_t *ui_card_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                         lv_coord_t w, lv_coord_t h,
                         const lv_image_dsc_t *icon, const char *label,
                         uint32_t color, lv_event_cb_t cb, void *arg)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_color(card, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 0, 0);

    if (cb != NULL) {
        ui_add_click_feedback(card);
        lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, arg);
    }

    /* 图标 + 文字整体在卡片内居中 */
    lv_coord_t icon_sz = 28;
    bool with_label = (label != NULL && label[0] != '\0');

    if (icon != NULL) {
        lv_obj_t *ic = lv_image_create(card);
        lv_image_set_src(ic, icon);
        lv_obj_set_style_image_recolor(ic, lv_color_hex(color), 0);
        lv_obj_set_style_radius(ic, 0, 0);
        lv_obj_align(ic, LV_ALIGN_CENTER, 0, with_label ? -(icon_sz / 2 + 2) : 0);
    }

    if (with_label) {
        lv_obj_t *lb = lv_label_create(card);
        lv_obj_set_style_text_color(lb, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(lb, &font_ui_14, 0);
        lv_label_set_text(lb, label);
        lv_obj_align(lb, LV_ALIGN_CENTER, 0, icon ? (icon_sz / 2 + 4) : 0);
    }

    return card;
}

/* ===================== 占位内容 ===================== */

void ui_placeholder_build(lv_obj_t *root, const char *title, const char *note,
                          lv_event_cb_t back_cb, void *back_arg)
{
    ui_titlebar_create(root, title, back_cb, back_arg);

    lv_obj_t *lb = lv_label_create(root);
    lv_obj_set_style_text_color(lb, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lb, &font_ui_14, 0);
    lv_label_set_text(lb, note ? note : "待实现");
    lv_obj_align(lb, LV_ALIGN_CENTER, 0, 10);
}

/* ===================== 设置列表 / 行（各二级设置页共用） ===================== */

lv_obj_t *ui_settings_list_create(lv_obj_t *root)
{
    /* 行数一多就会比一屏高，所以必须可滚动。
     * 纵向拖动滚列表，横向滑动仍然由页面手势接管（右滑返回）。 */
    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_size(list, UI_SCREEN_W, UI_SCREEN_H - UI_TITLEBAR_H);
    lv_obj_set_pos(list, 0, UI_TITLEBAR_H);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_bottom(list, UI_ROW_TOP, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    /* 细滚动条，别抢视觉 */
    lv_obj_set_style_width(list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(list, UI_COLOR_TEXT_DIM, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(list, 2, LV_PART_SCROLLBAR);

    return list;
}

lv_obj_t *ui_row_create(lv_obj_t *parent, lv_coord_t y,
                        const lv_image_dsc_t *icon, const char *name)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, UI_ROW_W, UI_ROW_H);
    lv_obj_set_pos(row, UI_ROW_X, y);
    lv_obj_set_style_bg_color(row, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_pad_all(row, 0, 0);

    if (icon != NULL) {
        lv_obj_t *ic = lv_image_create(row);
        lv_image_set_src(ic, icon);
        lv_obj_set_style_image_recolor(ic, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_radius(ic, 0, 0);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 8, 0);
    }

    if (name != NULL) {
        lv_obj_t *lb = lv_label_create(row);
        lv_obj_set_style_text_color(lb, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(lb, &font_ui_14, 0);
        lv_label_set_text(lb, name);
        /* 固定宽度 + 超长省略号：右侧还要放按钮，名称长了不能顶过去。
         * 150 是给「右侧最多两个按钮（约 130px）」留出来的余量。 */
        lv_obj_set_width(lb, 150);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
        lv_obj_align(lb, LV_ALIGN_LEFT_MID, icon ? 48 : 12, 0);
    }

    return row;
}

lv_obj_t *ui_row_value_create(lv_obj_t *row, lv_coord_t right_ofs, lv_coord_t w)
{
    lv_obj_t *lb = lv_label_create(row);
    lv_obj_set_style_text_color(lb, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(lb, &font_ui_14, 0);
    lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lb, w);
    lv_obj_align(lb, LV_ALIGN_RIGHT_MID, right_ofs, 0);
    return lb;
}

lv_obj_t *ui_row_btn_create(lv_obj_t *row, lv_coord_t right_ofs, lv_coord_t w,
                            const char *text, lv_event_cb_t cb, void *arg)
{
    lv_obj_t *btn = lv_button_create(row);
    lv_obj_set_size(btn, w, 30);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, right_ofs, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    /* 比行底色更深一点，看着像内嵌的按键 */
    lv_obj_set_style_bg_color(btn, UI_COLOR_BG, 0);

    if (cb != NULL) {
        ui_add_click_feedback(btn);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, arg);
    }

    lv_obj_t *lb = lv_label_create(btn);
    lv_obj_set_style_text_color(lb, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(lb, &font_ui_14, 0);
    lv_label_set_text(lb, text ? text : "");
    lv_obj_center(lb);

    return lb;
}

/*! 钮子开关尺寸 */
#define ROW_SWITCH_W    46
#define ROW_SWITCH_H    24

void ui_row_switch_set(lv_obj_t *sw, bool on)
{
    if (sw == NULL) {
        return;
    }
    /* 程序改状态不会触发 VALUE_CHANGED，也就不会反过来调业务回调 */
    if (on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
    }
}

lv_obj_t *ui_row_switch_create(lv_obj_t *row, bool on, lv_event_cb_t cb, void *arg)
{
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, ROW_SWITCH_W, ROW_SWITCH_H);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(sw, UI_COLOR_PRIMARY,
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_shadow_width(sw, 0, 0);
    ui_row_switch_set(sw, on);

    if (cb != NULL) {
        /* 点一下和拖动滑块都会发这个事件 */
        lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, arg);
    }
    return sw;
}

/* ===================== 电池图标（代码绘制） ===================== */

#define BATT_W          24
#define BATT_H          12
/*! 电量条满格宽度：外壳 24 宽、边框 1px，内部可用 22，左右各留 1px → 20 */
#define BATT_FILL_MAX   (BATT_W - 4)
/*! 整个状态栏图标的宽度：外壳占右边 26，剩给 "100%" 这种四位数字的得够宽，
 *  否则电量满格时数字会顶到外壳上 */
#define BATT_ROOT_W     70

static lv_obj_t *s_batt_root = NULL;
static lv_obj_t *s_batt_fill = NULL;
static lv_obj_t *s_batt_pct  = NULL;

/**
 * @brief 图标随页面销毁时把 static 指针置空
 *
 * 不置空的话，页面被 lv_obj_delete 之后这几个指针就是野指针，
 * 而 ui_battery_set() 只看指针非空就往里写 —— 释放掉的内存若被
 * 新页面的对象复用，就会去改别人的控件（表现为界面莫名其妙串内容）。
 * 必须比对事件目标，避免新页面已经建好图标时被旧页面的回调误清。
 */
static void batt_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    if (lv_event_get_target(e) != s_batt_root) {
        return;
    }
    s_batt_root = NULL;
    s_batt_fill = NULL;
    s_batt_pct  = NULL;
}

lv_obj_t *ui_battery_create(lv_obj_t *parent, lv_align_t align,
                            lv_coord_t x_ofs, lv_coord_t y_ofs)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(root, BATT_ROOT_W, 16);
    lv_obj_align(root, align, x_ofs, y_ofs);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);

    /* 百分比数字（靠左，用固定宽度对齐避免文字长度变化推动外壳） */
    s_batt_pct = lv_label_create(root);
    lv_obj_set_style_text_color(s_batt_pct, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_batt_pct, &font_ui_14, 0);
    lv_label_set_text(s_batt_pct, "--%");
    lv_obj_align(s_batt_pct, LV_ALIGN_LEFT_MID, 0, 0);

    /* 外壳 */
    lv_obj_t *body = lv_obj_create(root);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(body, BATT_W, BATT_H);
    lv_obj_align(body, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_border_color(body, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_radius(body, 3, 0);
    lv_obj_set_style_pad_all(body, 0, 0);

    /* 正极凸点 */
    lv_obj_t *cap = lv_obj_create(root);
    lv_obj_remove_flag(cap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(cap, 2, 5);
    lv_obj_align_to(cap, body, LV_ALIGN_OUT_RIGHT_MID, 1, 0);
    lv_obj_set_style_bg_color(cap, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cap, 0, 0);
    lv_obj_set_style_radius(cap, 1, 0);
    lv_obj_set_style_pad_all(cap, 0, 0);

    /* 电量填充条 */
    s_batt_fill = lv_obj_create(body);
    lv_obj_remove_flag(s_batt_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_batt_fill, BATT_FILL_MAX, BATT_H - 6);
    /* 偏移只能给 1：LV_ALIGN_LEFT_MID 是相对「内容区」对齐的，而内容区
     * 已经把 1px 边框算进去了（内容区从 body.x1+1 起）。原先写 2 就多让出
     * 一像素，电量条左边比右边宽 1px，看着不对称。 */
    lv_obj_align(s_batt_fill, LV_ALIGN_LEFT_MID, 1, 0);
    lv_obj_set_style_bg_color(s_batt_fill, UI_COLOR_OK, 0);
    lv_obj_set_style_bg_opa(s_batt_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_batt_fill, 0, 0);
    lv_obj_set_style_radius(s_batt_fill, 1, 0);
    lv_obj_set_style_pad_all(s_batt_fill, 0, 0);

    s_batt_root = root;
    lv_obj_add_event_cb(root, batt_del_cb, LV_EVENT_DELETE, NULL);
    ui_battery_set(0, false);          /* 默认隐藏，等真实数据 */

    return root;
}

void ui_battery_set(int pct, bool valid)
{
    if (s_batt_root == NULL) {
        return;
    }
    if (!valid) {
        lv_obj_add_flag(s_batt_root, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_batt_root, LV_OBJ_FLAG_HIDDEN);

    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    lv_coord_t w = (lv_coord_t)(BATT_FILL_MAX * pct / 100);
    if (pct > 0 && w < 2) {
        w = 2;                          /* 有电就至少留一点，避免看起来是空的 */
    }
    lv_obj_set_width(s_batt_fill, w);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(s_batt_pct, buf);

    /* 低电量变红 */
    lv_obj_set_style_bg_color(s_batt_fill,
                              (pct <= 20) ? UI_COLOR_TEMP : UI_COLOR_OK, 0);
}

/* ===================== SD 卡图标 ===================== */

static lv_obj_t *s_sd_img = NULL;

/* 同电池图标：页面销毁后必须把 static 指针置空，否则 ui_sd_icon_set() 会写野指针 */
static void sd_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    if (lv_event_get_target(e) != s_sd_img) {
        return;
    }
    s_sd_img = NULL;
}

lv_obj_t *ui_sd_icon_create(lv_obj_t *parent, lv_align_t align,
                            lv_coord_t x_ofs, lv_coord_t y_ofs)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, &ic_SD);
    lv_obj_set_style_image_recolor(img, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_radius(img, 0, 0);
    lv_obj_align(img, align, x_ofs, y_ofs);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);      /* 默认不显示 */

    s_sd_img = img;
    lv_obj_add_event_cb(img, sd_del_cb, LV_EVENT_DELETE, NULL);
    return img;
}

void ui_sd_icon_set(bool mounted)
{
    if (s_sd_img == NULL) {
        return;
    }
    /* 有卡显示图标，没卡直接不显示 */
    if (mounted) {
        lv_obj_remove_flag(s_sd_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_sd_img, LV_OBJ_FLAG_HIDDEN);
    }
}
