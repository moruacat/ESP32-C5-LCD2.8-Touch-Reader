#include "ui_set_pages.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "ui_dialog.h"
#include "app_navigator.h"
#include "srv_config.h"

#include <stdio.h>
#include <string.h>

/**
 * @brief 设置 → 照片（二级菜单）
 *
 * 目前只有「照片密码」一项：没设过直接弹数字键盘；设过了先问「修改 / 清除」，
 * 两个动作都要验旧密码。
 */

static lv_obj_t *s_state_lb;

typedef enum {
    PASS_ACT_SET = 0,   /*!< 首次设置（当前没密码） */
    PASS_ACT_CHANGE,    /*!< 修改密码（已验过旧密码） */
    PASS_ACT_CLEAR,     /*!< 清除密码（已验过旧密码） */
} pass_act_t;

static void refresh_state(void)
{
    if (s_state_lb == NULL) {
        return;
    }
    lv_label_set_text(s_state_lb,
                      (srv_config()->photo_pass[0] != '\0') ? "已设置" : "未设置");
}

static void ask_new_pass(pass_act_t act);

/* 旧密码验过了：按动作分派 */
static void on_verify_done(const char *text, void *arg)
{
    pass_act_t act = (pass_act_t)(intptr_t)arg;

    if (strcmp(text, srv_config()->photo_pass) != 0) {
        ui_alert_show("密码错误", UI_COLOR_TEMP, "请重新输入", "知道了",
                      NULL, NULL, 1800);
        return;
    }

    if (act == PASS_ACT_CLEAR) {
        srv_config_set_str(CFG_PHOTO_PASS, "", true);
        refresh_state();
        ui_alert_show("已清除", UI_COLOR_OK, "进入照片不再需要密码",
                      "知道了", NULL, NULL, 2000);
        return;
    }

    ask_new_pass(act);              /* 改密码：接着输新密码 */
}

static void on_new_pass_done(const char *text, void *arg)
{
    (void)arg;
    srv_config_set_str(CFG_PHOTO_PASS, text, true);
    refresh_state();
    ui_alert_show("已设置", UI_COLOR_OK, "进入照片时需要输入这 6 位数字",
                  "知道了", NULL, NULL, 2200);
}

static void ask_new_pass(pass_act_t act)
{
    ui_pass_input_show((act == PASS_ACT_SET) ? "设置照片密码" : "设置新密码",
                       on_new_pass_done, NULL);
}

static void on_pass_menu_cb(int idx, void *arg)
{
    (void)arg;
    if (idx == 0) {
        /* 改密码：先验旧的，验过才让输新的 */
        ui_pass_input_show("输入当前密码", on_verify_done,
                           (void *)(intptr_t)PASS_ACT_CHANGE);
    } else if (idx == 1) {
        /* 清密码同样要验旧的，验过就直接清（不再叠一层确认框） */
        ui_pass_input_show("输入当前密码", on_verify_done,
                           (void *)(intptr_t)PASS_ACT_CLEAR);
    }
}

static void open_pass_menu(void)
{
    if (srv_config()->photo_pass[0] == '\0') {
        ask_new_pass(PASS_ACT_SET);
        return;
    }

    static const ui_menu_item_t items[] = {
        {"修改密码", &ic_setting},
        {"清除密码", &ic_delete},
    };
    ui_menu_show("照片密码", items, 2, on_pass_menu_cb, NULL);
}

static void on_row_click(lv_event_t *e)
{
    (void)e;
    open_pass_menu();
}

void ui_set_photo_create(lv_obj_t *root)
{
    ui_titlebar_create(root, "照片", app_navigator_back_cb, NULL);
    lv_obj_t *list = ui_settings_list_create(root);

    lv_obj_t *row = ui_row_create(list, UI_ROW_Y(0), &ic_image, "照片密码");
    ui_add_click_feedback(row);
    lv_obj_add_event_cb(row, on_row_click, LV_EVENT_CLICKED, NULL);

    s_state_lb = lv_label_create(row);
    lv_obj_set_style_text_color(s_state_lb, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_state_lb, &font_ui_14, 0);
    lv_obj_set_width(s_state_lb, 90);
    lv_obj_set_style_text_align(s_state_lb, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_state_lb, LV_ALIGN_RIGHT_MID, -12, 0);

    refresh_state();
}
