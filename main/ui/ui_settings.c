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
    SET_PASS,           /*!< 点击设置/修改数字密码（照片） */
    SET_SECTION,        /*!< 分组标题：只显示文字，不可点 */
} set_kind_t;

typedef struct {
    const char           *name;
    const lv_image_dsc_t *icon;
    set_kind_t            kind;
    const char           *cfg_key;    /*!< SET_TOGGLE / SET_PASS 对应的配置键 */
    app_page_id_t         page;       /*!< SET_DETAIL 进入的页面 */
    bool                  hide_in_safe;   /*!< 安全模式下不列出（阅读/音乐/文件管理） */
} set_item_t;

static const set_item_t s_items[] = {
    {"网络", &ic_wifi,      SET_DETAIL, NULL, PAGE_SET_NET, false},
    {"时间", &ic_ntp,       SET_DETAIL, NULL, PAGE_SET_TIME, false},
    {"记录", &ic_page,      SET_TOGGLE, CFG_TEMP_REC_EN, PAGE_MAX, false},   /* 温湿度记录：开关存配置 */
    {"闹钟", &ic_timer,     SET_DETAIL, NULL, PAGE_SET_ALARM, false},
    {"阅读", &ic_book_open, SET_DETAIL, NULL, PAGE_SET_READ, true},
    {"音乐", &ic_music,     SET_DETAIL, NULL, PAGE_SET_MUSIC, true},
    {"文件管理", &ic_folder, SET_DETAIL, NULL, PAGE_FILES, true},
    {"照片密码", &ic_image, SET_PASS,   CFG_PHOTO_PASS, PAGE_MAX, true},
    {"电池", &ic_charging,  SET_DETAIL, NULL, PAGE_SET_BATTERY, false},
    {"亮度", &ic_brightness, SET_DETAIL, NULL, PAGE_SET_BRIGHT, false},
    /* 安全模式开着时连「快速返回主页」也收起来：这页里留着的都该是
     * 跟书/音乐/照片无关的东西 */
    {"快速返回主页", &ic_home, SET_TOGGLE, CFG_FAST_HOME, PAGE_MAX, true},
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
    const set_item_t *it = &s_items[idx];

    if (it->kind == SET_PASS) {
        /* 密码不回显，只说有没有设过 */
        snprintf(buf, n, "%s",
                 (srv_config()->photo_pass[0] != '\0') ? "已设置" : "未设置");
        return true;
    }

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

/* ===================== 照片密码 ===================== */

/* 密码键盘的回调只带一个 void*，把「动作 + 设置项下标」打包传过去 */
typedef enum {
    PASS_ACT_SET = 0,   /*!< 首次设置（当前没密码） */
    PASS_ACT_CHANGE,    /*!< 修改密码（已验过旧密码） */
    PASS_ACT_CLEAR,     /*!< 清除密码（已验过旧密码） */
} pass_act_t;

static void *pass_arg_pack(pass_act_t act, int idx)
{
    return (void *)(intptr_t)(((int)act << 8) | idx);
}

static int pass_arg_idx(void *arg)
{
    return (int)(intptr_t)arg & 0xFF;
}

static pass_act_t pass_arg_act(void *arg)
{
    return (pass_act_t)(((int)(intptr_t)arg >> 8) & 0xFF);
}

static void ask_new_pass(pass_act_t act, int idx);

/* 旧密码验过了：按动作分派 */
static void on_verify_done(const char *text, void *arg)
{
    pass_act_t act = pass_arg_act(arg);
    int        idx = pass_arg_idx(arg);

    if (strcmp(text, srv_config()->photo_pass) != 0) {
        ui_alert_show("密码错误", UI_COLOR_TEMP, "请重新输入", "知道了",
                      NULL, NULL, 1800);
        return;
    }

    if (act == PASS_ACT_CLEAR) {
        srv_config_set_str(CFG_PHOTO_PASS, "", true);
        refresh_item(idx);
        ui_alert_show("已清除", UI_COLOR_OK, "进入照片不再需要密码",
                      "知道了", NULL, NULL, 2000);
        return;
    }

    ask_new_pass(act, idx);         /* 改密码：接着输新密码 */
}

static void on_new_pass_done(const char *text, void *arg)
{
    int idx = pass_arg_idx(arg);

    srv_config_set_str(CFG_PHOTO_PASS, text, true);
    refresh_item(idx);
    ui_alert_show("已设置", UI_COLOR_OK, "进入照片时需要输入这 6 位数字",
                  "知道了", NULL, NULL, 2200);
}

static void ask_new_pass(pass_act_t act, int idx)
{
    ui_pass_input_show((act == PASS_ACT_SET) ? "设置照片密码" : "设置新密码",
                       on_new_pass_done, pass_arg_pack(act, idx));
}

/* 菜单回调只带 idx 和 arg，用 arg 把「设置项下标」带过去 */
static void on_pass_menu_cb(int idx, void *arg)
{
    int item = (int)(intptr_t)arg;

    if (idx == 0) {
        /* 改密码：先验旧的，验过才让输新的 */
        ui_pass_input_show("输入当前密码", on_verify_done,
                           pass_arg_pack(PASS_ACT_CHANGE, item));
    } else if (idx == 1) {
        /* 清密码同样要验旧的，验过就直接清（不再叠一层确认框） */
        ui_pass_input_show("输入当前密码", on_verify_done,
                           pass_arg_pack(PASS_ACT_CLEAR, item));
    }
}

/* 密码项：没设过就直接弹键盘，设过了先问要改还是清除 */
static void open_pass_menu(int idx)
{
    if (srv_config()->photo_pass[0] == '\0') {
        ask_new_pass(PASS_ACT_SET, idx);
        return;
    }

    static const ui_menu_item_t items[] = {
        {"修改密码", &ic_setting},
        {"清除密码", &ic_delete},
    };
    ui_menu_show("照片密码", items, 2, on_pass_menu_cb, (void *)(intptr_t)idx);
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
    if (it->kind == SET_PASS) {
        open_pass_menu(idx);
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
    lv_obj_set_width(state, 90);
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

/* ===================== 安全模式开关（只有完整版设置里有） ===================== */

static void on_safe_switch(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    const bool want = lv_obj_has_state(sw, LV_STATE_CHECKED);

    if (want == srv_safe_on()) {
        return;                     /* 程序同步状态时触发的，不是用户改的 */
    }

    const bool on = srv_safe_toggle();
    /* 标题里点一句「重启后保持」：开关是落盘的，免得重启后东西还藏着让人意外 */
    ui_alert_show(on ? "安全模式已开启（重启后保持）" : "安全模式已关闭",
                  on ? UI_COLOR_OK : UI_COLOR_TEXT_DIM,
                  on ? "阅读 / 音乐 / 文件 / 照片密码\n快速返回主页 已收起"
                     : "普通设置恢复完整",
                  "知道了", NULL, NULL, on ? 3200 : 1800);
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
    ui_titlebar_create(root, s_full ? "完整版设置" : "设置", app_navigator_back_cb, NULL);

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

    /* 完整版设置：末尾多一行「安全模式」开关（普通设置里没有，
     * 它是唯一能开/关安全模式的地方） */
    if (s_full) {
        lv_obj_t *row = ui_row_create(list, y, &ic_warning, "安全模式");
        (void)ui_row_switch_create(row, srv_safe_on(), on_safe_switch, NULL);
    }
}