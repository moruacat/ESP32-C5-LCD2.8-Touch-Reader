#include "ui_dialog.h"
#include "ui_common.h"
#include "srv_config.h"         /* 照片密码的位数/缓冲长度宏 */

/* 封面用 lv_image 显示，走的是 LVGL 的图片缓存。缓存以「dsc 地址」为键，
 * 而封面层复用一个 static dsc，所以关掉封面时必须把缓存条目也丢掉 ——
 * 否则下次点开另一本封面会命中旧条目（甚至指向已释放的像素缓冲）。 */
#include "misc/cache/instance/lv_image_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 滚轮最多列数：日期需要 年/月/日/时/分 五列，留一列余量 */
#define PICKER_MAX_COLS  6
/*! 选项字符串缓冲：亮度 1~100 这一类单列 100 项也放得下。 */
#define PICKER_OPT_MAX   512
/*! 面板最小宽度：底部要放得下「取消 + 确定」两个按钮（96*2 + 边距） */
#define PANEL_MIN_W      232

/* 同一时刻只允许存在一个弹层，所以结果回调和列参数用 static 存是安全的。
 * 注意：不能在回调里再弹一个选择器（会覆盖这两个 static），
 * 目前的用法都是「选完直接写配置」，没有嵌套。 */
static ui_dialog_ok_cb_t s_ok_cb;
static void             *s_ok_arg;

static lv_obj_t         *s_pick_roller[PICKER_MAX_COLS];
static int               s_pick_start[PICKER_MAX_COLS];
static int               s_pick_cols;
static ui_picker_done_cb_t s_pick_cb;
static void             *s_pick_arg;

static lv_obj_t         *s_num_mask;
static lv_obj_t         *s_num_ta;
static int               s_num_min;
static int               s_num_max;
static ui_number_input_cb_t s_num_cb;
static void             *s_num_arg;
static ui_number_input_cancel_cb_t s_num_cancel_cb;
static void             *s_num_cancel_arg;

static lv_obj_t      *s_alert_mask;
static ui_alert_cb_t  s_alert_cb;
static void          *s_alert_arg;

static lv_obj_t      *s_menu_mask;
static ui_menu_cb_t   s_menu_cb;
static void          *s_menu_arg;

static lv_obj_t      *s_info_mask;

static lv_obj_t      *s_cover_mask;
static uint8_t       *s_cover_data;      /*!< 封面像素缓冲，随弹层一起释放 */
static lv_image_dsc_t s_cover_dsc;       /*!< 复用的图片描述符（缓存键是它的地址） */

/* ===================== 共用骨架 ===================== */

/* 全屏遮罩：吃掉点击，避免穿透到底下的按钮 */
static lv_obj_t *mask_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    if (scr == NULL) {
        return NULL;
    }

    lv_obj_t *mask = lv_obj_create(scr);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(mask, ui_scr_w(), ui_scr_h());
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_50, 0);
    lv_obj_set_style_border_width(mask, 0, 0);
    lv_obj_set_style_radius(mask, 0, 0);
    lv_obj_set_style_pad_all(mask, 0, 0);
    return mask;
}

static lv_obj_t *panel_create(lv_obj_t *mask, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *box = lv_obj_create(mask);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(box, w, h);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    return box;
}

static void close_cb(lv_event_t *e)
{
    lv_obj_t *mask = (lv_obj_t *)lv_event_get_user_data(e);
    if (mask != NULL) {
        lv_obj_delete(mask);
    }
}

/* 面板里的文字（标题/说明）
 *
 * 必须限宽 + 限行数：label 默认宽度是「按文字撑开」，长书名这类内容会溢出
 * 面板；而对象又是 TOP_MID 居中摆的，溢出后的结果不是"从左边被切掉"，而是
 * **看到中间一截**，看着像乱码。限了宽高之后，超出的部分才会按
 * LV_LABEL_LONG_DOT 收成省略号。
 *
 * 宽度直接从父面板取（各面板都是显式尺寸），省得每个调用点再抄一遍数字。
 *
 * @param lines 最多显示几行，超出的收成省略号
 */
static void add_text(lv_obj_t *box, const char *text, lv_color_t color,
                     lv_coord_t y, int lines)
{
    if (text == NULL || text[0] == '\0' || lines <= 0) {
        return;
    }

    lv_obj_t *lb = lv_label_create(box);
    lv_obj_set_style_text_font(lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(lb, color, 0);
    /* 先给文字再定尺寸：lv_label 的文档要求 LONG_DOT/WRAP 这类模式在
     * 设完文字之后再定宽高，否则这次排版不一定按新尺寸算 */
    lv_label_set_text(lb, text);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lb, lv_obj_get_style_width(box, LV_PART_MAIN) - 16);
    /* 行高 +2 是给字形的上下余量，卡太紧最后一行会被裁掉一点 */
    lv_obj_set_height(lb, (lv_coord_t)(lv_font_get_line_height(&font_ui_14) + 2) * lines);
    lv_obj_align(lb, LV_ALIGN_TOP_MID, 0, y);
}

/* 面板底部的按钮。primary=true 用主色实心（更显眼）。
 * x_ofs/y_ofs 是相对 align 指定位置的偏移，一般 y 给负数（往上抬） */
static lv_obj_t *add_btn(lv_obj_t *box, lv_align_t align, lv_coord_t x_ofs, lv_coord_t y_ofs,
                         lv_coord_t w, bool primary, const char *text,
                         lv_event_cb_t cb, void *arg)
{
    lv_obj_t *btn = lv_button_create(box);
    lv_obj_set_size(btn, w, 36);
    lv_obj_align(btn, align, x_ofs, y_ofs);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, primary ? UI_COLOR_PRIMARY : UI_COLOR_BG, 0);
    ui_add_click_feedback(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, arg);

    lv_obj_t *lb = lv_label_create(btn);
    lv_obj_set_style_text_font(lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(lb, primary ? lv_color_white() : UI_COLOR_TEXT, 0);
    lv_label_set_text(lb, text ? text : "");
    lv_obj_center(lb);

    return btn;
}

/* 底部左右两个按钮：宽度跟着面板走，窄面板上不能写死 96 否则会重合 */
static void add_btn_pair(lv_obj_t *box, lv_coord_t panel_w,
                         const char *left_text, lv_event_cb_t left_cb, void *left_arg,
                         const char *right_text, lv_event_cb_t right_cb, void *right_arg,
                         bool right_primary)
{
    lv_coord_t w = (lv_coord_t)((panel_w - 36) / 2);
    if (w > 96) {
        w = 96;
    }
    add_btn(box, LV_ALIGN_BOTTOM_LEFT, 12, -12, w, !right_primary,
            left_text, left_cb, left_arg);
    add_btn(box, LV_ALIGN_BOTTOM_RIGHT, -12, -12, w, right_primary,
            right_text, right_cb, right_arg);
}

/* 危险动作按钮：红色实心 + 白字。取消按钮仍沿用原来的主色样式。 */
static lv_obj_t *add_btn_danger(lv_obj_t *box, lv_align_t align,
                                lv_coord_t x_ofs, lv_coord_t y_ofs,
                                lv_coord_t w, const char *text,
                                lv_event_cb_t cb, void *arg)
{
    lv_obj_t *btn = add_btn(box, align, x_ofs, y_ofs, w, false, text, cb, arg);
    lv_obj_set_style_bg_color(btn, UI_COLOR_TEMP, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_t *lb = lv_obj_get_child(btn, 0);
    if (lb != NULL) {
        lv_obj_set_style_text_color(lb, lv_color_white(), 0);
    }
    return btn;
}

/* ===================== 确认对话框 ===================== */

static void confirm_ok_cb(lv_event_t *e)
{
    lv_obj_t *mask = (lv_obj_t *)lv_event_get_user_data(e);
    ui_dialog_ok_cb_t cb = s_ok_cb;
    void *arg = s_ok_arg;

    s_ok_cb  = NULL;
    s_ok_arg = NULL;

    /* 先关掉面板再回调：回调里常常要重建整页内容（例如换 SD 卡），
     * 留着遮罩会挡住新内容，而且遮罩挂在屏幕上、不随页面内容一起更新 */
    if (mask != NULL) {
        lv_obj_delete(mask);
    }

    if (cb != NULL) {
        cb(arg);
    }
}

void ui_dialog_confirm(const char *title, const char *note, const char *ok_text,
                       ui_dialog_ok_cb_t ok_cb, void *arg)
{
    lv_obj_t *mask = mask_create();
    if (mask == NULL) {
        return;
    }

    s_ok_cb  = ok_cb;
    s_ok_arg = arg;

    lv_obj_t *box = panel_create(mask, (ui_scr_w() < 252) ? (ui_scr_w() - 8) : 244, 134);
    add_text(box, title, UI_COLOR_TEMP, 18, 1);
    add_text(box, note, UI_COLOR_TEXT_DIM, 46, 2);

    /* 取消仍在右侧、保持原来的主色；真正执行动作的按钮改成红色警示。 */
    lv_coord_t w = (lv_obj_get_width(box) - 36) / 2;
    if (w > 96) {
        w = 96;
    }
    add_btn_danger(box, LV_ALIGN_BOTTOM_LEFT, 12, -12, w,
                   ok_text ? ok_text : "确定", confirm_ok_cb, mask);
    add_btn(box, LV_ALIGN_BOTTOM_RIGHT, -12, -12, w, true,
            "取消", close_cb, mask);
}

/* ===================== 提示层 ===================== */

static void alert_auto_close_cb(lv_timer_t *timer)
{
    lv_obj_t *mask = (lv_obj_t *)lv_timer_get_user_data(timer);
    if (mask != NULL) {
        lv_obj_delete(mask);        /* 触发下面的 del 回调删掉本定时器 */
    }
}

static void alert_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }

    lv_timer_t *t = (lv_timer_t *)lv_event_get_user_data(e);
    if (t != NULL) {
        lv_timer_delete(t);
    }

    if (lv_event_get_target(e) == s_alert_mask) {
        s_alert_mask = NULL;
        s_alert_cb   = NULL;
        s_alert_arg  = NULL;
    }
}

static void alert_btn_cb(lv_event_t *e)
{
    lv_obj_t *mask = (lv_obj_t *)lv_event_get_user_data(e);
    ui_alert_cb_t cb = s_alert_cb;
    void *arg = s_alert_arg;

    if (mask != NULL) {
        lv_obj_delete(mask);        /* 会清掉 s_alert_* */
    }
    if (cb != NULL) {
        cb(arg);
    }
}

void ui_alert_show(const char *title, lv_color_t title_color, const char *note,
                   const char *btn_text, ui_alert_cb_t cb, void *arg,
                   uint32_t auto_close_ms)
{
    ui_alert_close();               /* 不允许叠层 */

    lv_obj_t *mask = mask_create();
    if (mask == NULL) {
        return;
    }

    s_alert_mask = mask;
    s_alert_cb   = cb;
    s_alert_arg  = arg;

    lv_obj_t *box = panel_create(mask, (ui_scr_w() < 244) ? (ui_scr_w() - 8) : 236, 142);
    add_text(box, title, title_color, 22, 1);
    add_text(box, note, UI_COLOR_TEXT_DIM, 52, 2);

    add_btn(box, LV_ALIGN_BOTTOM_MID, 0, -12, 140, true,
            btn_text ? btn_text : "知道了", alert_btn_cb, mask);

    if (auto_close_ms > 0) {
        lv_timer_t *t = lv_timer_create(alert_auto_close_cb, auto_close_ms, mask);
        lv_obj_add_event_cb(mask, alert_del_cb, LV_EVENT_DELETE, t);
    } else {
        lv_obj_add_event_cb(mask, alert_del_cb, LV_EVENT_DELETE, NULL);
    }
}

void ui_alert_close(void)
{
    if (s_alert_mask != NULL && lv_obj_is_valid(s_alert_mask)) {
        lv_obj_delete(s_alert_mask);
    }
}

bool ui_alert_visible(void)
{
    return s_alert_mask != NULL && lv_obj_is_valid(s_alert_mask);
}

/* ===================== 多列滚轮选择器 ===================== */

static void picker_ok_cb(lv_event_t *e)
{
    lv_obj_t *mask = (lv_obj_t *)lv_event_get_user_data(e);

    int values[PICKER_MAX_COLS];
    for (int i = 0; i < s_pick_cols; i++) {
        values[i] = s_pick_start[i] + (int)lv_roller_get_selected(s_pick_roller[i]);
    }

    ui_picker_done_cb_t cb = s_pick_cb;
    void *arg = s_pick_arg;
    s_pick_cb = NULL;
    s_pick_arg = NULL;

    if (mask != NULL) {
        lv_obj_delete(mask);
    }

    if (cb != NULL) {
        cb(values, arg);
    }
}

/* 生成 "start\nstart+1\n...\n" 形式的选项 */
static void fill_options(char *buf, size_t cap, int start, int count)
{
    size_t p = 0;

    for (int i = 0; i < count; i++) {
        if (i > 0 && p < cap) {
            buf[p++] = '\n';
        }
        if (p >= cap) {
            break;
        }
        p += (size_t)snprintf(buf + p, cap - p, "%d", start + i);
    }
    if (cap > 0) {
        buf[p < cap ? p : cap - 1] = '\0';
    }
}

void ui_picker_show(const char *title, int col_count, const ui_picker_col_t *cols,
                    ui_picker_done_cb_t cb, void *arg)
{
    if (cols == NULL || col_count <= 0 || col_count > PICKER_MAX_COLS) {
        return;
    }

    lv_obj_t *mask = mask_create();
    if (mask == NULL) {
        return;
    }

    /* 列宽：先按可用宽度均分，再限制上限（列少时别拉太宽） */
    const int gap   = 6;
    const int avail = 288;
    int       col_w = (avail - gap * (col_count - 1)) / col_count;
    if (col_w > 80) {
        col_w = 80;
    }
    const int total_w = col_w * col_count + gap * (col_count - 1);

    /* 面板不能窄过底部两个按钮所需要的宽度，否则「取消」和「确定」会叠在一起 */
    int panel_w = total_w + 24;
    if (panel_w < PANEL_MIN_W) {
        panel_w = PANEL_MIN_W;
    }

    lv_obj_t *box = panel_create(mask, (lv_coord_t)panel_w, 202);
    add_text(box, title, UI_COLOR_TEXT, 14, 1);

    s_pick_cols = col_count;
    s_pick_cb   = cb;
    s_pick_arg  = arg;

    const lv_coord_t x0 = (lv_coord_t)((panel_w - total_w) / 2);

    for (int i = 0; i < col_count; i++) {
        char buf[PICKER_OPT_MAX];
        fill_options(buf, sizeof(buf), cols[i].start, cols[i].count);

        lv_obj_t *r = lv_roller_create(box);
        lv_roller_set_options(r, buf, LV_ROLLER_MODE_INFINITE);
        lv_obj_set_width(r, col_w);
        lv_obj_set_pos(r, x0 + (col_w + gap) * i, 44);
        lv_obj_set_style_bg_color(r, UI_COLOR_BG, 0);
        lv_obj_set_style_border_width(r, 0, 0);
        lv_obj_set_style_radius(r, 8, 0);
        lv_obj_set_style_pad_all(r, 0, 0);
        lv_obj_set_style_text_font(r, &font_ui_14, 0);
        lv_obj_set_style_text_color(r, UI_COLOR_TEXT_DIM, 0);
        /* 行距要在算高度之前设好 */
        lv_obj_set_style_text_line_space(r, 6, 0);
        lv_roller_set_visible_row_count(r, 3);
        lv_obj_set_style_bg_color(r, UI_COLOR_PRIMARY, LV_PART_SELECTED);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_SELECTED);
        lv_obj_set_style_text_color(r, lv_color_white(), LV_PART_SELECTED);
        lv_obj_set_style_radius(r, 6, LV_PART_SELECTED);

        /* 当前值夹进合法范围 */
        int sel = cols[i].value - cols[i].start;
        if (sel < 0) {
            sel = 0;
        }
        if (sel >= cols[i].count) {
            sel = cols[i].count - 1;
        }
        lv_roller_set_selected(r, (uint32_t)sel, LV_ANIM_OFF);

        s_pick_roller[i] = r;
        s_pick_start[i]  = cols[i].start;

        if (cols[i].label != NULL) {
            lv_obj_t *lb = lv_label_create(box);
            lv_obj_set_style_text_font(lb, &font_ui_14, 0);
            lv_obj_set_style_text_color(lb, UI_COLOR_TEXT_DIM, 0);
            lv_label_set_text(lb, cols[i].label);
            lv_obj_align_to(lb, r, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
        }
    }

    add_btn_pair(box, (lv_coord_t)panel_w,
                 "取消", close_cb, mask,
                 "确定", picker_ok_cb, mask, true);
}

/* ===================== 数值输入层（数字键盘） ===================== */

#define NUM_PANEL_H  66
#define NUM_KB_TOP   74
#define NUM_KB_H     160
#define NUM_TEXT_MAX 8

static void num_close(void)
{
    if (s_num_mask != NULL && lv_obj_is_valid(s_num_mask)) {
        lv_obj_delete(s_num_mask);
    }
}

static void num_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    if (lv_event_get_target(e) != s_num_mask) {
        return;
    }
    s_num_mask = NULL;
    s_num_ta   = NULL;
    s_num_cb   = NULL;
    s_num_arg  = NULL;
    s_num_cancel_cb  = NULL;
    s_num_cancel_arg = NULL;
}

static void num_submit(void)
{
    if (s_num_ta == NULL || !lv_obj_is_valid(s_num_ta)) {
        return;
    }

    long value = strtol(lv_textarea_get_text(s_num_ta), NULL, 10);
    if (value < s_num_min) {
        value = s_num_min;
    }
    if (value > s_num_max) {
        value = s_num_max;
    }

    ui_number_input_cb_t cb = s_num_cb;
    void *arg = s_num_arg;
    s_num_cancel_cb  = NULL;
    s_num_cancel_arg = NULL;
    num_close();
    if (cb != NULL) {
        cb((int)value, arg);
    }
}

static void num_ok_cb(lv_event_t *e)
{
    (void)e;
    num_submit();
}

static void num_cancel_cb(lv_event_t *e)
{
    (void)e;
    ui_number_input_cancel_cb_t cancel = s_num_cancel_cb;
    void *cancel_arg = s_num_cancel_arg;
    s_num_cancel_cb  = NULL;
    s_num_cancel_arg = NULL;
    num_close();
    if (cancel != NULL) {
        cancel(cancel_arg);
    }
}

static void num_ready_cb(lv_event_t *e)
{
    (void)e;
    num_submit();
}

void ui_number_input_show_ex(const char *title, int min_value, int max_value,
                             int initial_value, ui_number_input_cb_t cb, void *arg,
                             ui_number_input_cancel_cb_t cancel_cb, void *cancel_arg)
{
    if (min_value > max_value) {
        int tmp = min_value;
        min_value = max_value;
        max_value = tmp;
    }

    ui_alert_close();
    num_close();

    lv_obj_t *mask = mask_create();
    if (mask == NULL) {
        return;
    }
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, 0);

    s_num_mask = mask;
    s_num_min  = min_value;
    s_num_max  = max_value;
    s_num_cb         = cb;
    s_num_arg        = arg;
    s_num_cancel_cb  = cancel_cb;
    s_num_cancel_arg = cancel_arg;
    lv_obj_add_event_cb(mask, num_del_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *box = panel_create(mask, ui_scr_w() - 16, NUM_PANEL_H);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *ttl = lv_label_create(box);
    lv_obj_set_style_text_font(ttl, &font_ui_14, 0);
    lv_obj_set_style_text_color(ttl, UI_COLOR_TEXT, 0);
    lv_obj_set_width(ttl, lv_obj_get_width(box) - 130);
    lv_label_set_long_mode(ttl, LV_LABEL_LONG_DOT);
    lv_label_set_text(ttl, (title != NULL) ? title : "输入数值");
    lv_obj_align(ttl, LV_ALIGN_TOP_LEFT, 10, 5);

    lv_obj_t *cancel = lv_button_create(box);
    lv_obj_set_size(cancel, 52, 24);
    lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -8, 5);
    lv_obj_set_style_radius(cancel, 6, 0);
    lv_obj_set_style_bg_color(cancel, UI_COLOR_BG, 0);
    lv_obj_set_style_shadow_width(cancel, 0, 0);
    lv_obj_set_style_pad_all(cancel, 0, 0);
    ui_add_click_feedback(cancel);
    lv_obj_add_event_cb(cancel, num_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *clb = lv_label_create(cancel);
    lv_obj_set_style_text_font(clb, &font_ui_14, 0);
    lv_obj_set_style_text_color(clb, UI_COLOR_TEXT, 0);
    lv_label_set_text(clb, "取消");
    lv_obj_center(clb);

    lv_obj_t *ok = lv_button_create(box);
    lv_obj_set_size(ok, 52, 24);
    lv_obj_align(ok, LV_ALIGN_TOP_RIGHT, -64, 5);
    lv_obj_set_style_radius(ok, 6, 0);
    lv_obj_set_style_bg_color(ok, UI_COLOR_PRIMARY, 0);
    lv_obj_set_style_shadow_width(ok, 0, 0);
    lv_obj_set_style_pad_all(ok, 0, 0);
    ui_add_click_feedback(ok);
    lv_obj_add_event_cb(ok, num_ok_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *olb = lv_label_create(ok);
    lv_obj_set_style_text_font(olb, &font_ui_14, 0);
    lv_obj_set_style_text_color(olb, lv_color_white(), 0);
    lv_label_set_text(olb, "确定");
    lv_obj_center(olb);

    lv_obj_t *ta = lv_textarea_create(box);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, NUM_TEXT_MAX);
    lv_textarea_set_accepted_chars(ta, "0123456789+-");
    lv_obj_set_size(ta, ui_scr_w() - 16 - 20, 26);
    lv_obj_align(ta, LV_ALIGN_BOTTOM_LEFT, 10, -7);
    lv_obj_set_style_bg_color(ta, UI_COLOR_BG, 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_radius(ta, 6, 0);
    lv_obj_set_style_text_font(ta, &font_ui_14, 0);
    lv_obj_set_style_text_color(ta, UI_COLOR_TEXT, 0);
    lv_obj_set_style_pad_all(ta, 3, 0);
    lv_obj_add_event_cb(ta, num_ready_cb, LV_EVENT_READY, NULL);
    s_num_ta = ta;

    char initial[16];
    snprintf(initial, sizeof(initial), "%d", initial_value);
    lv_textarea_set_text(ta, initial);
    lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);

    lv_obj_t *kb = lv_keyboard_create(mask);
    lv_obj_set_size(kb, ui_scr_w() - 16, NUM_KB_H);
    lv_obj_align(kb, LV_ALIGN_TOP_MID, 0, NUM_KB_TOP);
    lv_obj_set_style_bg_opa(kb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_radius(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 2, 0);
    lv_obj_set_style_pad_row(kb, 3, 0);
    lv_obj_set_style_pad_column(kb, 3, 0);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(kb, UI_COLOR_SURFACE, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, UI_COLOR_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 5, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, UI_COLOR_PRIMARY,
                              (lv_style_selector_t)(LV_PART_ITEMS | LV_STATE_PRESSED));
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(kb, ta);
}

void ui_number_input_show(const char *title, int min_value, int max_value,
                          int initial_value, ui_number_input_cb_t cb, void *arg)
{
    ui_number_input_show_ex(title, min_value, max_value, initial_value, cb, arg, NULL, NULL);
}

/* ===================== 文字输入层（标题 + 输入框 + 屏幕键盘） ===================== */

/*! 键盘区顶边与高度：240 高的屏，5 行键、每行约 28px */
#define INPUT_KB_TOP     74
#define INPUT_KB_H       160
/*! 输入框内容上限（缓冲区的栈开销也在 LVGL 任务上，别开太大） */
#define INPUT_TEXT_MAX   128

/* 键盘布局。
 * LVGL 自带的键盘一行塞 12 个键，320 宽的屏上单键只有 23px，手指按不准，
 * 而且数字要切到「1#」页才有。这里重排成每行 10 个单位（单键约 30px），
 * 数字单独占一行常驻。
 * 单位宽度写在 ctrl 表里：同一行内宽度之和相等，各行才是等宽的键。
 * 字母区后面的 @ . - _ 是密码里最常出现的符号，放在手边省得切页。 */
#define INPUT_FN_BTN(w)  (LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT | (w))

static const char *const kb_map_lc[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "@", "\n",
    "z", "x", "c", "v", "b", "n", "m", ".", "-", "_", "\n",
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "ABC", "1#", " ", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""
};

static const lv_buttonmatrix_ctrl_t kb_ctrl_lc[] = {
    /* q w e r t y u i o p */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* a s d f g h j k l @ */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* z x c v b n m . - _ */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* 1 2 3 4 5 6 7 8 9 0 */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* 大写 符号 空格 退格 确定 */
    INPUT_FN_BTN(1), INPUT_FN_BTN(1), 4, 2, INPUT_FN_BTN(2),
};

static const char *const kb_map_uc[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "@", "\n",
    "Z", "X", "C", "V", "B", "N", "M", ".", "-", "_", "\n",
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "abc", "1#", " ", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""
};

static const lv_buttonmatrix_ctrl_t kb_ctrl_uc[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    INPUT_FN_BTN(1), INPUT_FN_BTN(1), 4, 2, INPUT_FN_BTN(2),
};

/* 「1#」页：够用的符号 + 数字，WiFi 密码里的特殊字符基本都在这 */
static const char *const kb_map_sp[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "\n",
    "-", "_", "=", "+", "[", "]", "{", "}", ";", ":", "\n",
    ",", ".", "/", "\\", "|", "'", "\"", "~", "`", "?", "\n",
    "abc", LV_SYMBOL_BACKSPACE, " ", LV_SYMBOL_OK, ""
};

static const lv_buttonmatrix_ctrl_t kb_ctrl_sp[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    INPUT_FN_BTN(1), 2, 5, INPUT_FN_BTN(2),
};

static ui_text_input_cb_t s_input_cb;
static void              *s_input_arg;
static lv_obj_t          *s_input_mask;
static lv_obj_t          *s_input_ta;

/* 「清空」按钮：一键抹掉输入框内容 */
static void input_clear_cb(lv_event_t *e)
{
    (void)e;
    if (s_input_ta != NULL && lv_obj_is_valid(s_input_ta)) {
        lv_textarea_set_text(s_input_ta, "");
        lv_textarea_set_cursor_pos(s_input_ta, 0);
    }
}

static void input_close(void)
{
    if (s_input_mask != NULL && lv_obj_is_valid(s_input_mask)) {
        lv_obj_delete(s_input_mask);        /* 触发 del 回调，清掉下面几个 static */
    }
}

static void input_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    if (lv_event_get_target(e) != s_input_mask) {
        return;                             /* 已经是新的输入层了，别清 */
    }
    s_input_mask = NULL;
    s_input_ta   = NULL;
    s_input_cb   = NULL;
    s_input_arg  = NULL;
}

static void input_cancel_cb(lv_event_t *e)
{
    (void)e;
    input_close();
}

/**
 * @brief 键盘上的「确定」键把 LV_EVENT_READY 转给了输入框
 */
static void input_ready_cb(lv_event_t *e)
{
    (void)e;

    /* 先把文字抄出来：弹层一关输入框就没了 */
    char text[INPUT_TEXT_MAX + 1];
    text[0] = '\0';
    if (s_input_ta != NULL) {
        snprintf(text, sizeof(text), "%s", lv_textarea_get_text(s_input_ta));
    }

    ui_text_input_cb_t cb = s_input_cb;
    void *arg = s_input_arg;

    input_close();
    if (cb != NULL) {
        cb(text, arg);
    }
}

void ui_text_input_show_init(const char *title, uint32_t max_len, const char *initial,
                             ui_text_input_cb_t cb, void *arg)
{
    /* 同时只留一个弹层 */
    ui_alert_close();
    input_close();

    if (max_len == 0 || max_len > INPUT_TEXT_MAX) {
        max_len = INPUT_TEXT_MAX;
    }

    lv_obj_t *mask = mask_create();
    if (mask == NULL) {
        return;
    }
    /* 比通用弹层更暗一点：底下压着整块键盘，太亮会抢注意力 */
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, 0);

    s_input_mask = mask;
    s_input_cb   = cb;
    s_input_arg  = arg;
    lv_obj_add_event_cb(mask, input_del_cb, LV_EVENT_DELETE, NULL);

    /* 顶部：标题 + 取消 + 输入框 */
    lv_obj_t *box = panel_create(mask, ui_scr_w() - 16, 66);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *ttl = lv_label_create(box);
    lv_obj_set_style_text_font(ttl, &font_ui_14, 0);
    lv_obj_set_style_text_color(ttl, UI_COLOR_TEXT, 0);
    lv_label_set_text(ttl, (title != NULL) ? title : "");
    lv_obj_set_width(ttl, lv_obj_get_width(box) - 76);
    lv_label_set_long_mode(ttl, LV_LABEL_LONG_DOT);
    lv_obj_align(ttl, LV_ALIGN_TOP_LEFT, 10, 6);

    lv_obj_t *cancel = lv_button_create(box);
    lv_obj_set_size(cancel, 52, 24);
    lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -8, 5);
    lv_obj_set_style_radius(cancel, 6, 0);
    lv_obj_set_style_bg_color(cancel, UI_COLOR_BG, 0);
    lv_obj_set_style_shadow_width(cancel, 0, 0);
    lv_obj_set_style_pad_all(cancel, 0, 0);
    ui_add_click_feedback(cancel);
    lv_obj_add_event_cb(cancel, input_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *clb = lv_label_create(cancel);
    lv_obj_set_style_text_font(clb, &font_ui_14, 0);
    lv_obj_set_style_text_color(clb, UI_COLOR_TEXT, 0);
    lv_label_set_text(clb, "取消");
    lv_obj_center(clb);

    /* 「清空」：一键抹掉输入框，不用长按退格 */
    lv_obj_t *clr = lv_button_create(box);
    lv_obj_set_size(clr, 52, 24);
    lv_obj_align(clr, LV_ALIGN_TOP_RIGHT, -64, 5);
    lv_obj_set_style_radius(clr, 6, 0);
    lv_obj_set_style_bg_color(clr, UI_COLOR_BG, 0);
    lv_obj_set_style_shadow_width(clr, 0, 0);
    lv_obj_set_style_pad_all(clr, 0, 0);
    ui_add_click_feedback(clr);
    lv_obj_add_event_cb(clr, input_clear_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *clr_lb = lv_label_create(clr);
    lv_obj_set_style_text_font(clr_lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(clr_lb, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(clr_lb, "清空");
    lv_obj_center(clr_lb);

    lv_obj_t *ta = lv_textarea_create(box);
    lv_textarea_set_one_line(ta, true);         /* 单行：不打回车，只滚动显示 */
    lv_textarea_set_max_length(ta, max_len);
    lv_obj_set_size(ta, ui_scr_w() - 16 - 20, 26);
    lv_obj_align(ta, LV_ALIGN_BOTTOM_LEFT, 10, -7);
    lv_obj_set_style_bg_color(ta, UI_COLOR_BG, 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_radius(ta, 6, 0);
    lv_obj_set_style_text_font(ta, &font_ui_14, 0);
    lv_obj_set_style_text_color(ta, UI_COLOR_TEXT, 0);
    lv_obj_set_style_pad_all(ta, 3, 0);
    lv_obj_add_event_cb(ta, input_ready_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(ta, input_cancel_cb, LV_EVENT_CANCEL, NULL);
    s_input_ta = ta;

    /* 预填上次的内容（查词框再次点开时保留单词/句子，方便改而不是重打） */
    if (initial != NULL && initial[0] != '\0') {
        lv_textarea_set_text(ta, initial);
        lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
    }

    lv_obj_t *kb = lv_keyboard_create(mask);
    lv_obj_set_size(kb, ui_scr_w() - 16, INPUT_KB_H);
    lv_obj_align(kb, LV_ALIGN_TOP_MID, 0, INPUT_KB_TOP);
    lv_obj_set_style_bg_opa(kb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_radius(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 2, 0);
    lv_obj_set_style_pad_row(kb, 3, 0);
    lv_obj_set_style_pad_column(kb, 3, 0);
    /* 按键上的退格/确定是 LVGL 图标（私用区码点），UI 字体是按汉字表生成的、
     * 不含这些字形，直接用会画出空白键 —— 换成内置的 montserrat */
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(kb, UI_COLOR_SURFACE, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, UI_COLOR_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 5, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, UI_COLOR_PRIMARY, (lv_style_selector_t)(LV_PART_ITEMS | LV_STATE_PRESSED));

    /* 三张表都要换成自己的：键盘上 abc / ABC / 1# 这三个键切的就是这三张表 */
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, kb_map_lc, kb_ctrl_lc);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, kb_map_uc, kb_ctrl_uc);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL, kb_map_sp, kb_ctrl_sp);

    lv_keyboard_set_textarea(kb, ta);           /* 输入框拿到焦点，光标开始闪 */
}

/* 兼容旧调用：不预填 */
void ui_text_input_show(const char *title, uint32_t max_len,
                        ui_text_input_cb_t cb, void *arg)
{
    ui_text_input_show_init(title, max_len, NULL, cb, arg);
}

/* ===================== 数字密码输入层（纯数字键盘） ===================== */

static ui_text_input_cb_t s_pass_cb;
static void              *s_pass_arg;
static lv_obj_t          *s_pass_mask;
static lv_obj_t          *s_pass_dots[SRV_CONFIG_PHOTO_PASS_DIGITS];
static char               s_pass_buf[SRV_CONFIG_PHOTO_PASS_LEN];

/* 按下即触发（不等松手）、且按住不重复发 —— 否则按住一个数字键会连插满 6 位自动提交 */
#define NUM_KEY     (LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT | 1)

static void pass_close(void)
{
    if (s_pass_mask != NULL && lv_obj_is_valid(s_pass_mask)) {
        lv_obj_delete(s_pass_mask);
    }
}

/* 把已输入的位数反映到圆点上 */
static void pass_dots_update(void)
{
    size_t n = strlen(s_pass_buf);
    for (int i = 0; i < SRV_CONFIG_PHOTO_PASS_DIGITS; i++) {
        if (s_pass_dots[i] == NULL || !lv_obj_is_valid(s_pass_dots[i])) {
            continue;
        }
        bool on = ((size_t)i < n);
        lv_obj_set_style_bg_color(s_pass_dots[i],
                                  on ? UI_COLOR_PRIMARY : UI_COLOR_SURFACE, 0);
    }
}

static void pass_submit(void)
{
    char text[SRV_CONFIG_PHOTO_PASS_LEN];
    snprintf(text, sizeof(text), "%s", s_pass_buf);

    ui_text_input_cb_t cb = s_pass_cb;
    void *arg = s_pass_arg;

    pass_close();
    if (cb != NULL) {
        cb(text, arg);
    }
}

static void pass_key_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    uint32_t id = lv_buttonmatrix_get_selected_button(lv_event_get_target(e));
    const char *txt = lv_buttonmatrix_get_button_text(lv_event_get_target(e), id);
    if (txt == NULL) {
        return;
    }

    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        size_t n = strlen(s_pass_buf);
        if (n > 0) {
            s_pass_buf[n - 1] = '\0';
        }
    } else if (strcmp(txt, LV_SYMBOL_OK) == 0) {
        if (strlen(s_pass_buf) == SRV_CONFIG_PHOTO_PASS_DIGITS) {
            pass_submit();
        }
        return;
    } else if (txt[0] >= '0' && txt[0] <= '9') {
        size_t n = strlen(s_pass_buf);
        if (n < SRV_CONFIG_PHOTO_PASS_DIGITS) {
            s_pass_buf[n]     = txt[0];
            s_pass_buf[n + 1] = '\0';
        }
    }

    pass_dots_update();

    /* 输满 6 位自动提交，省得再按一次确定 */
    if (strlen(s_pass_buf) == SRV_CONFIG_PHOTO_PASS_DIGITS) {
        pass_submit();
    }
}

static void pass_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    if (lv_event_get_target(e) != s_pass_mask) {
        return;
    }
    s_pass_mask = NULL;
    s_pass_cb   = NULL;
    s_pass_arg  = NULL;
    for (int i = 0; i < SRV_CONFIG_PHOTO_PASS_DIGITS; i++) {
        s_pass_dots[i] = NULL;
    }
}

static void pass_cancel_cb(lv_event_t *e)
{
    (void)e;
    pass_close();
}

void ui_pass_input_show(const char *title, ui_text_input_cb_t cb, void *arg)
{
    ui_alert_close();
    pass_close();

    s_pass_buf[0] = '\0';

    lv_obj_t *mask = mask_create();
    if (mask == NULL) {
        return;
    }
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, 0);

    s_pass_mask = mask;
    s_pass_cb   = cb;
    s_pass_arg  = arg;
    lv_obj_add_event_cb(mask, pass_del_cb, LV_EVENT_DELETE, NULL);

    /* 顶部：标题 + 取消 + 密码圆点 */
    lv_obj_t *box = panel_create(mask, ui_scr_w() - 16, 66);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *ttl = lv_label_create(box);
    lv_obj_set_style_text_font(ttl, &font_ui_14, 0);
    lv_obj_set_style_text_color(ttl, UI_COLOR_TEXT, 0);
    lv_label_set_text(ttl, (title != NULL) ? title : "输入密码");
    lv_obj_set_width(ttl, lv_obj_get_width(box) - 76);
    lv_label_set_long_mode(ttl, LV_LABEL_LONG_DOT);
    lv_obj_align(ttl, LV_ALIGN_TOP_LEFT, 10, 6);

    lv_obj_t *cancel = lv_button_create(box);
    lv_obj_set_size(cancel, 52, 24);
    lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -8, 5);
    lv_obj_set_style_radius(cancel, 6, 0);
    lv_obj_set_style_bg_color(cancel, UI_COLOR_BG, 0);
    lv_obj_set_style_shadow_width(cancel, 0, 0);
    lv_obj_set_style_pad_all(cancel, 0, 0);
    ui_add_click_feedback(cancel);
    lv_obj_add_event_cb(cancel, pass_cancel_cb, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *clb = lv_label_create(cancel);
        lv_obj_set_style_text_font(clb, &font_ui_14, 0);
        lv_obj_set_style_text_color(clb, UI_COLOR_TEXT, 0);
        lv_label_set_text(clb, "取消");
        lv_obj_center(clb);
    }

    /* 密码圆点：一排 6 个，已输入的填主题色 */
    for (int i = 0; i < SRV_CONFIG_PHOTO_PASS_DIGITS; i++) {
        lv_obj_t *dot = lv_obj_create(box);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(dot, 14, 14);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, UI_COLOR_SURFACE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        /* 6 个点居中排开：间距 22，整体宽度按点数对称偏移 */
        lv_obj_align(dot, LV_ALIGN_BOTTOM_MID,
                     (lv_coord_t)(i * 22 - (SRV_CONFIG_PHOTO_PASS_DIGITS - 1) * 22 / 2), -6);
        s_pass_dots[i] = dot;
    }

    /* 数字键盘：铺满圆点以下到屏幕底部 */
    lv_obj_t *kb = lv_buttonmatrix_create(mask);
    static const char *const map[] = {
        "1", "2", "3", "\n",
        "4", "5", "6", "\n",
        "7", "8", "9", "\n",
        LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK, ""
    };
    static const lv_buttonmatrix_ctrl_t ctrl[] = {
        NUM_KEY, NUM_KEY, NUM_KEY,
        NUM_KEY, NUM_KEY, NUM_KEY,
        NUM_KEY, NUM_KEY, NUM_KEY,
        NUM_KEY, NUM_KEY, NUM_KEY,
    };
    lv_buttonmatrix_set_map(kb, map);
    lv_buttonmatrix_set_ctrl_map(kb, ctrl);
    lv_obj_set_size(kb, ui_scr_w() - 16, 152);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_opa(kb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_radius(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 2, 0);
    lv_obj_set_style_pad_row(kb, 3, 0);
    lv_obj_set_style_pad_column(kb, 3, 0);
    /* 退格/确定是 LVGL 图标（私用区码点），UI 字体按汉字表生成、不含这些字形 */
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(kb, UI_COLOR_SURFACE, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, UI_COLOR_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 5, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, UI_COLOR_PRIMARY,
                              (lv_style_selector_t)(LV_PART_ITEMS | LV_STATE_PRESSED));
    lv_obj_add_event_cb(kb, pass_key_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ===================== 操作菜单 ===================== */

/* 普通单列菜单沿用紧凑行高，最多 6 项；
 * 阅读器这种长按菜单改用下面的 ui_menu_show_2col()，不再压扁按钮。 */
#define MENU_PANEL_W   ((ui_scr_w() > 276) ? 268 : (ui_scr_w() - 8))
#define MENU_TITLE_H   28
#define MENU_ITEM_H    30
#define MENU_ITEM_GAP  3

static void menu_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    /* 手快「关一个又开一个」时新菜单已经建好，旧回调别把新指针清掉 */
    if (lv_event_get_target(e) != s_menu_mask) {
        return;
    }
    s_menu_mask = NULL;
    s_menu_cb   = NULL;
    s_menu_arg  = NULL;
}

/* 关掉菜单再回调：idx = -1 表示取消（或点了空白处） */
static void menu_finish(int idx)
{
    lv_obj_t    *mask = s_menu_mask;
    ui_menu_cb_t cb   = s_menu_cb;
    void        *arg  = s_menu_arg;

    if (mask != NULL && lv_obj_is_valid(mask)) {
        lv_obj_delete(mask);        /* 触发 del 回调，清掉上面几个 static */
    } else {
        s_menu_mask = NULL;
        s_menu_cb   = NULL;
        s_menu_arg  = NULL;
    }

    if (cb != NULL) {
        cb(idx, arg);
    }
}

static void menu_item_cb(lv_event_t *e)
{
    menu_finish((int)(intptr_t)lv_event_get_user_data(e));
}

static void menu_blank_cb(lv_event_t *e)
{
    (void)e;
    menu_finish(-1);
}

/* 菜单里的一行：可选图标 + 文字 */
static void menu_add_row(lv_obj_t *box, lv_coord_t y, const ui_menu_item_t *it,
                         int idx, lv_event_cb_t cb, bool dim)
{
    lv_obj_t *row = lv_button_create(box);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, MENU_PANEL_W - 24, MENU_ITEM_H);
    lv_obj_set_pos(row, 12, y);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_bg_color(row, UI_COLOR_BG, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    ui_add_click_feedback(row);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    lv_obj_t *lb = lv_label_create(row);
    lv_obj_set_style_text_font(lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(lb, dim ? UI_COLOR_TEXT_DIM : UI_COLOR_TEXT, 0);
    lv_label_set_text(lb, (it->text != NULL) ? it->text : "");

    if (it->icon != NULL) {
        lv_obj_t *ic = lv_image_create(row);
        lv_image_set_src(ic, it->icon);
        lv_obj_set_style_image_recolor(ic, dim ? UI_COLOR_TEXT_DIM : UI_COLOR_TEXT, 0);
        lv_obj_set_style_radius(ic, 0, 0);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 10, 0);

        /* 文字从图标右边起排，长文字省略号收尾，别顶到右边缘 */
        lv_obj_set_width(lb, MENU_PANEL_W - 24 - 48 - 12);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
        lv_obj_align(lb, LV_ALIGN_LEFT_MID, 48, 0);
    } else {
        lv_obj_center(lb);
    }
}

void ui_menu_show(const char *title, const ui_menu_item_t *items, int count,
                  ui_menu_cb_t cb, void *arg)
{
    if (items == NULL || count <= 0 || count > UI_MENU_MAX_ITEMS) {
        return;
    }

    /* 同一时刻只留一个弹层 */
    ui_alert_close();
    if (s_menu_mask != NULL && lv_obj_is_valid(s_menu_mask)) {
        lv_obj_delete(s_menu_mask);
    }

    lv_obj_t *mask = mask_create();
    if (mask == NULL) {
        return;
    }

    s_menu_mask = mask;
    s_menu_cb   = cb;
    s_menu_arg  = arg;
    lv_obj_add_event_cb(mask, menu_del_cb, LV_EVENT_DELETE, NULL);
    /* 点遮罩空白处等同取消（落在面板上的点击被面板自己吃掉，不会冒泡到这） */
    lv_obj_add_event_cb(mask, menu_blank_cb, LV_EVENT_CLICKED, NULL);

    const lv_coord_t pw = MENU_PANEL_W;
    const lv_coord_t ph = MENU_TITLE_H + (lv_coord_t)count * (MENU_ITEM_H + MENU_ITEM_GAP)
                          + MENU_ITEM_H + 8;
    lv_obj_t *box = panel_create(mask, pw, ph);

    if (title != NULL && title[0] != '\0') {
        lv_obj_t *ttl = lv_label_create(box);
        lv_obj_set_style_text_font(ttl, &font_ui_14, 0);
        lv_obj_set_style_text_color(ttl, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_width(ttl, pw - 24);
        lv_label_set_long_mode(ttl, LV_LABEL_LONG_DOT);
        lv_label_set_text(ttl, title);
        lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 10);
    }

    for (int i = 0; i < count; i++) {
        menu_add_row(box, MENU_TITLE_H + (lv_coord_t)i * (MENU_ITEM_H + MENU_ITEM_GAP),
                     &items[i], i, menu_item_cb, false);
    }

    /* 「取消」固定排最后，点它回调 -1 */
    const ui_menu_item_t cancel = { "取消", NULL };
    menu_add_row(box, MENU_TITLE_H + (lv_coord_t)count * (MENU_ITEM_H + MENU_ITEM_GAP),
                 &cancel, -1, menu_item_cb, true);
}

/* ===================== 两列操作菜单 ===================== */

#define MENU_2COL_W       ((ui_scr_w() > 320) ? 312 : (ui_scr_w() - 8))
#define MENU_2COL_MARGIN  6
#define MENU_2COL_GAP     4
#define MENU_2COL_ITEM_H  34

/* 一个菜单格子：可选图标 + 文字。文字超长时省略，不顶到格子外。 */
static void menu_add_cell(lv_obj_t *box, lv_coord_t x, lv_coord_t y,
                          lv_coord_t w, const ui_menu_item_t *it,
                          int idx, lv_event_cb_t cb, bool dim)
{
    lv_obj_t *cell = lv_button_create(box);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(cell, w, MENU_2COL_ITEM_H);
    lv_obj_set_pos(cell, x, y);
    lv_obj_set_style_radius(cell, 8, 0);
    lv_obj_set_style_bg_color(cell, UI_COLOR_BG, 0);
    lv_obj_set_style_shadow_width(cell, 0, 0);
    lv_obj_set_style_pad_all(cell, 0, 0);
    ui_add_click_feedback(cell);
    lv_obj_add_event_cb(cell, cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    lv_obj_t *lb = lv_label_create(cell);
    lv_obj_set_style_text_font(lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(lb, dim ? UI_COLOR_TEXT_DIM : UI_COLOR_TEXT, 0);
    lv_label_set_text(lb, (it->text != NULL) ? it->text : "");

    if (it->icon != NULL) {
        lv_obj_t *ic = lv_image_create(cell);
        lv_image_set_src(ic, it->icon);
        lv_obj_set_style_image_recolor(ic, dim ? UI_COLOR_TEXT_DIM : UI_COLOR_TEXT, 0);
        lv_obj_set_style_radius(ic, 0, 0);
        /* 两列菜单的格子窄，图标缩到 20px，给文字留出正常宽度。 */
        lv_image_set_scale(ic, 20 * 256 / 32);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 8, 0);

        lv_obj_set_width(lb, w - 42);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
        lv_obj_align(lb, LV_ALIGN_LEFT_MID, 34, 0);
    } else {
        lv_obj_center(lb);
    }
}

void ui_menu_show_2col(const char *title, const ui_menu_item_t *items, int count,
                       ui_menu_cb_t cb, void *arg)
{
    if (items == NULL || count <= 0 || count > UI_MENU_MAX_ITEMS) {
        return;
    }

    ui_alert_close();
    if (s_menu_mask != NULL && lv_obj_is_valid(s_menu_mask)) {
        lv_obj_delete(s_menu_mask);
    }

    lv_obj_t *mask = mask_create();
    if (mask == NULL) {
        return;
    }

    s_menu_mask = mask;
    s_menu_cb   = cb;
    s_menu_arg  = arg;
    lv_obj_add_event_cb(mask, menu_del_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(mask, menu_blank_cb, LV_EVENT_CLICKED, NULL);

    const lv_coord_t cell_w = (MENU_2COL_W - MENU_2COL_MARGIN * 2 - MENU_2COL_GAP) / 2;
    const int item_rows = (count + 1) / 2;
    const lv_coord_t panel_h = MENU_TITLE_H
        + (lv_coord_t)item_rows * (MENU_2COL_ITEM_H + MENU_2COL_GAP)
        + MENU_2COL_ITEM_H + 8;
    lv_obj_t *box = panel_create(mask, MENU_2COL_W, panel_h);

    if (title != NULL && title[0] != '\0') {
        lv_obj_t *ttl = lv_label_create(box);
        lv_obj_set_style_text_font(ttl, &font_ui_14, 0);
        lv_obj_set_style_text_color(ttl, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_width(ttl, MENU_2COL_W - 24);
        lv_label_set_long_mode(ttl, LV_LABEL_LONG_DOT);
        lv_label_set_text(ttl, title);
        lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 10);
    }

    for (int i = 0; i < count; i++) {
        int row = i / 2;
        int col = i % 2;
        lv_coord_t x = MENU_2COL_MARGIN + col * (cell_w + MENU_2COL_GAP);
        lv_coord_t y = MENU_TITLE_H + row * (MENU_2COL_ITEM_H + MENU_2COL_GAP);
        menu_add_cell(box, x, y, cell_w, &items[i], i, menu_item_cb, false);
    }

    const ui_menu_item_t cancel = { "取消", NULL };
    lv_coord_t cancel_y = MENU_TITLE_H + item_rows * (MENU_2COL_ITEM_H + MENU_2COL_GAP);
    menu_add_cell(box, MENU_2COL_MARGIN, cancel_y,
                  MENU_2COL_W - MENU_2COL_MARGIN * 2, &cancel, -1,
                  menu_item_cb, true);
}

/* ===================== 长文本层 ===================== */

/* 简介可能上千字节，面板能给多大给多大：整屏减去一圈 8px 边距。
 * 原来只有 280x196（正文区 110px ≈ 6 行），看一段几百字的简介要滑半天。 */
#define INFO_PANEL_W   (ui_scr_w() - 16)
#define INFO_PANEL_H   (ui_scr_h() - 16)
#define INFO_TITLE_Y   4
#define INFO_TITLE_H   26      /* 标题一行（含行高余量），正文从它下面开始 */
#define INFO_BODY_TOP  INFO_TITLE_H
#define INFO_BTN_H     36
#define INFO_BTN_GAP   10
/* 正文区高度 = 面板高 - 标题 - 底部按钮（含它到面板底的距离） - 一点余量 */
#define INFO_BODY_H    (INFO_PANEL_H - INFO_BODY_TOP - INFO_BTN_H - INFO_BTN_GAP - 8)

static void info_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    if (lv_event_get_target(e) != s_info_mask) {
        return;
    }
    s_info_mask = NULL;
}

void ui_info_show(const char *title, const char *text)
{
    ui_alert_close();
    if (s_menu_mask != NULL && lv_obj_is_valid(s_menu_mask)) {
        lv_obj_delete(s_menu_mask);
    }

    lv_obj_t *mask = mask_create();
    if (mask == NULL) {
        return;
    }

    s_info_mask = mask;
    lv_obj_add_event_cb(mask, info_del_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *box = panel_create(mask, INFO_PANEL_W, INFO_PANEL_H);
    add_text(box, title, UI_COLOR_TEXT_DIM, INFO_TITLE_Y, 1);

    /* 正文容器：固定高度、可纵向滚动，长简介不会把面板撑破。
     * 内边距留 6（不是 8）：正文区能多放一点，且左右各让出滚动条的位置 */
    lv_obj_t *cont = lv_obj_create(box);
    lv_obj_set_size(cont, INFO_PANEL_W - 12, INFO_BODY_H);
    lv_obj_set_pos(cont, 6, INFO_BODY_TOP);
    lv_obj_set_style_bg_color(cont, UI_COLOR_BG, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_radius(cont, 8, 0);
    lv_obj_set_style_pad_all(cont, 6, 0);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *lb = lv_label_create(cont);
    lv_obj_set_width(lb, INFO_PANEL_W - 12 - 12 - 8);   /* 去掉内边距，再让一点给滚动条 */
    lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(lb, UI_COLOR_TEXT, 0);
    /* 简介里的换行原样保留（转换工具会把段落折行写进去），一段一行好读 */
    lv_label_set_text(lb, (text != NULL && text[0] != '\0') ? text : "（无内容）");

    add_btn(box, LV_ALIGN_BOTTOM_MID, 0, -INFO_BTN_GAP, 96, true, "关闭", close_cb, mask);
}

/* ===================== 封面层 ===================== */

/*! 封面旋转角度（lv_image 的单位是 0.1 度）。
 *  面板物理是竖的，逻辑 x 轴才是物理的竖直方向，而 BSP 的 swap+mirror 让
 *  「物理顶部」落在「逻辑左侧」，所以逆时针 90°（= 2700）竖起来看才是正的。
 *  真机上若封面上下颠倒，把它改成 900 即可。 */
#define COVER_ROTATION  2700

static void cover_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    if (lv_event_get_target(e) != s_cover_mask) {
        return;
    }
    s_cover_mask = NULL;

    if (s_cover_data != NULL) {
        /* 先丢缓存条目再还缓冲：缓存里存的是指向这块内存的指针，
         * 留在那儿下次同地址命中就成了野指针 */
        lv_image_cache_drop(&s_cover_dsc);
        free(s_cover_data);
        s_cover_data = NULL;
    }
}

void ui_cover_show(uint8_t *data, size_t bytes, int32_t w, int32_t h)
{
    if (data == NULL || bytes == 0 || w <= 0 || h <= 0) {
        free(data);                     /* 本函数接管所有权，提前返回也得还回去 */
        return;
    }

    ui_alert_close();
    if (s_menu_mask != NULL && lv_obj_is_valid(s_menu_mask)) {
        lv_obj_delete(s_menu_mask);
    }
    if (s_info_mask != NULL && lv_obj_is_valid(s_info_mask)) {
        lv_obj_delete(s_info_mask);
    }

    lv_obj_t *mask = mask_create();
    if (mask == NULL) {
        free(data);
        return;
    }
    /* 全屏看图，底下的阅读器/书架一点都不露 */
    lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, 0);

    s_cover_mask = mask;
    s_cover_data = data;
    lv_obj_add_event_cb(mask, cover_del_cb, LV_EVENT_DELETE, NULL);
    /* 点任意位置关闭（图片不可点击，点击都落在遮罩上） */
    lv_obj_add_event_cb(mask, close_cb, LV_EVENT_CLICKED, mask);

    /* 转 90° 之后，图像的视觉宽 = 原图高、视觉高 = 原图宽，缩放要按这个算。
     * 封面通常比屏幕小，所以允许放大到铺满（全屏看图，不做「小图不放大」）。 */
    uint32_t sx = (uint32_t)(((int64_t)ui_scr_w() * 256) / h);
    uint32_t sy = (uint32_t)(((int64_t)ui_scr_h() * 256) / w);
    uint32_t scale = (sx < sy) ? sx : sy;
    if (scale == 0) {
        scale = 1;
    }

    memset(&s_cover_dsc, 0, sizeof(s_cover_dsc));
    s_cover_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_cover_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    s_cover_dsc.header.w      = (uint32_t)w;
    s_cover_dsc.header.h      = (uint32_t)h;
    s_cover_dsc.header.stride = (uint32_t)w * 2;   /* RGB565：每像素 2 字节 */
    s_cover_dsc.data_size     = (uint32_t)bytes;
    s_cover_dsc.data          = data;

    lv_obj_t *img = lv_image_create(mask);
    lv_image_set_src(img, &s_cover_dsc);
    /* 对象尺寸用「缩放后的原图尺寸」：图像默认居中填满它，再整体旋转。
     * 旋转后宽高互换、会超出对象边界，LVGL 按 ext_draw_size 照样画出来，
     * 而遮罩就是整屏，裁不到它。 */
    lv_obj_set_size(img, (lv_coord_t)((int64_t)w * scale / 256),
                         (lv_coord_t)((int64_t)h * scale / 256));
    lv_image_set_scale(img, scale);
    lv_image_set_rotation(img, COVER_ROTATION);
    lv_obj_center(img);
}
