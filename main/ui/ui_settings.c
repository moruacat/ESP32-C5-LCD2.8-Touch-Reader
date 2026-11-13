#include "ui_settings.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "ui_dialog.h"
#include "app_navigator.h"
#include "srv_config.h"
#include "srv_sensors.h"
#include "srv_safe.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/**
 * @brief 设置项类型
 */
typedef enum {
    SET_TOGGLE = 0,     /*!< 行右侧是钮子开关 */
    SET_DETAIL,         /*!< 点击进入二级页 */
    SET_SECTION,        /*!< 分组标题：只显示文字，不可点 */
} set_kind_t;

typedef struct {
    const char           *name;
    const lv_image_dsc_t *icon;
    set_kind_t            kind;
    const char           *cfg_key;    /*!< SET_TOGGLE 对应的配置键 */
    app_page_id_t         page;       /*!< SET_DETAIL 进入的页面 */
    bool                  hide_in_safe;   /*!< 安全模式下不列出（阅读/音乐/文件管理） */
} set_item_t;

static const set_item_t s_items[] = {
    {"网络", &ic_wifi,      SET_DETAIL, NULL, PAGE_SET_NET, false},
    {"时间", &ic_ntp,       SET_DETAIL, NULL, PAGE_SET_TIME, false},
    {"记录", &ic_page,      SET_DETAIL, NULL, PAGE_SET_RECORD, false},      /* 记录二级页 */
    {"闹钟", &ic_timer,     SET_DETAIL, NULL, PAGE_SET_ALARM, false},
    {"阅读", &ic_book_open, SET_DETAIL, NULL, PAGE_SET_READ, true},
    {"音乐", &ic_music,     SET_DETAIL, NULL, PAGE_SET_MUSIC, true},
    {"文件", &ic_folder,    SET_DETAIL, NULL, PAGE_FILES, true},
    /* 照片相关设置（照片密码）收在「照片」二级页里 */
    {"照片", &ic_image,     SET_DETAIL, NULL, PAGE_SET_PHOTO, true},
    {"电池", &ic_charging,  SET_DETAIL, NULL, PAGE_SET_BATTERY, false},
    {"亮度", &ic_brightness, SET_DETAIL, NULL, PAGE_SET_BRIGHT, false},
    {"关于", &ic_info,      SET_DETAIL, NULL, PAGE_SET_ABOUT, false},
};

#define ITEM_COUNT  (sizeof(s_items) / sizeof(s_items[0]))

/*! 是不是「完整版设置」（安全菜单右侧进的那个）：完整版永远全显示，末尾多一行安全模式开关 */
static bool s_full = false;

static void ui_settings_build(lv_obj_t *root);   /* 先声明：两个入口都调它 */

static lv_obj_t *s_state_lb[ITEM_COUNT];
static lv_obj_t *s_arrow_ic[ITEM_COUNT];
static lv_obj_t *s_sw[ITEM_COUNT];       /*!< SET_TOGGLE 项右侧的钮子开关 */

/**
 * @brief 取某项右侧要显示的状态文字
 * @return false 表示没有文字，改为显示「可进入」箭头
 */
static bool item_state_text(int idx, char *buf, size_t n)
{
    (void)idx;
    (void)buf;
    (void)n;
    return false;                       /* 二级页一律显示箭头 */
}

static void refresh_item(int idx)
{
    const set_item_t *it = &s_items[idx];

    if (it->kind == SET_TOGGLE) {
        ui_row_switch_set(s_sw[idx], srv_config_get_int(it->cfg_key, 0) != 0);
        return;
    }

    char buf[24];
    bool has_text = item_state_text(idx, buf, sizeof(buf));

    if (has_text) {
        lv_label_set_text(s_state_lb[idx], buf);
        lv_obj_remove_flag(s_state_lb[idx], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_arrow_ic[idx], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_state_lb[idx], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_arrow_ic[idx], LV_OBJ_FLAG_HIDDEN);
    }
}

static void apply_toggle(int idx, bool on)
{
    const set_item_t *it = &s_items[idx];

    srv_config_set_int(it->cfg_key, on ? 1 : 0, true);

    /* 温湿度记录：除了存开关，还得真的启停采样任务 */
    if (strcmp(it->cfg_key, CFG_TEMP_REC_EN) == 0) {
        if (on) {
            srv_sensors_record_start(10);
        } else {
            srv_sensors_record_stop();
        }
    }
}

/* ===================== 行交互 ===================== */

/* 点整行也能切换（行的可点区域比开关大，好按） */
static void toggle_item(int idx)
{
    if (idx < 0 || idx >= (int)ITEM_COUNT || s_items[idx].kind != SET_TOGGLE) {
        return;
    }

    bool on = (srv_config_get_int(s_items[idx].cfg_key, 0) == 0);
    apply_toggle(idx, on);
    refresh_item(idx);
}

static void on_row_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= (int)ITEM_COUNT) {
        return;
    }

    const set_item_t *it = &s_items[idx];

    if (it->kind == SET_DETAIL) {
        app_navigator_switch(it->page, true);
        return;
    }

    toggle_item(idx);
}

/* 拖动/点击开关本身 */
static void on_row_switch(lv_event_t *e)
{
    toggle_item((int)(intptr_t)lv_event_get_user_data(e));
}

static void build_row(lv_obj_t *parent, int idx, lv_coord_t y)
{
    const set_item_t *it = &s_items[idx];

    /* 分组标题：只一行灰字，不建行对象、不注册点击 */
    if (it->kind == SET_SECTION) {
        lv_obj_t *sec = lv_label_create(parent);
        lv_obj_set_style_text_color(sec, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_font(sec, &font_ui_14, 0);
        lv_obj_set_pos(sec, 12, y + 4);
        lv_label_set_text(sec, it->name);
        return;
    }

    lv_obj_t *row = ui_row_create(parent, y, it->icon, it->name);
    ui_add_click_feedback(row);
    lv_obj_add_event_cb(row, on_row_click, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    if (it->kind == SET_TOGGLE) {
        s_sw[idx] = ui_row_switch_create(row,
                                         srv_config_get_int(it->cfg_key, 0) != 0,
                                         on_row_switch, (void *)(intptr_t)idx);
        refresh_item(idx);
        return;
    }

    /* 右侧：状态文字 或 「可进入」箭头，两者只显示一个 */
    lv_obj_t *state = lv_label_create(row);
    lv_obj_set_style_text_color(state, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(state, &font_ui_14, 0);
    lv_obj_set_width(state, lv_pct(30));   /* 竖屏行窄，按比例给右侧状态让位 */
    lv_label_set_long_mode(state, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(state, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(state, LV_ALIGN_RIGHT_MID, -12, 0);
    s_state_lb[idx] = state;

    lv_obj_t *arrow = lv_image_create(row);
    lv_image_set_src(arrow, &ic_arrow_right);
    lv_obj_set_style_image_recolor(arrow, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_radius(arrow, 0, 0);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -10, 0);
    s_arrow_ic[idx] = arrow;

    refresh_item(idx);
}

/* ===================== 构建 ===================== */

void ui_settings_create(lv_obj_t *root)
{
    s_full = false;
    ui_settings_build(root);
}

void ui_settings_full_create(lv_obj_t *root)
{
    s_full = true;
    ui_settings_build(root);
}

static void ui_settings_build(lv_obj_t *root)
{
    ui_titlebar_create(root, "设置", app_navigator_back_cb, NULL);

    /* 九个设置项按 40px 行高排下来比一屏还高，所以塞进可滚动的列表容器 ——
     * 行高不能为了"一屏塞下"而压缩，那样 32x32 的图标会被挤出行外。
     * 纵向拖动滚动，横向滑动仍然由页面的手势接管（返回上一级）。 */
    lv_obj_t *list = ui_settings_list_create(root);

    /* 安全模式开着时把 hide_in_safe 的项收起来（完整版设置不受影响）；
     * 行号按实际显示顺序排 */
    const bool hide_some = (!s_full) && srv_safe_on();

    lv_coord_t y = 0;
    for (int i = 0; i < (int)ITEM_COUNT; i++) {
        if (s_items[i].hide_in_safe && hide_some) {
            continue;
        }
        build_row(list, i, y);
        y += UI_ROW_H + UI_ROW_GAP;
    }

    /* 安全模式开关已经挪到「关于」页，这一页不再有额外的行 */
}