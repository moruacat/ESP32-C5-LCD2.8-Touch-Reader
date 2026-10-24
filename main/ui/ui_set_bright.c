#include "ui_set_pages.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "app_navigator.h"
#include "srv_config.h"

#include <stdint.h>
#include <stdio.h>

/*! 变暗时间每次加减多少秒（10 秒一档，10~600 秒） */
#define DIM_STEP    10

static lv_obj_t *s_dim_lb;      /*!< 自动降低亮度开关 */
static lv_obj_t *s_time_lb;     /*!< 变暗时间数值 */

static void bright_refresh(void)
{
    ui_row_switch_set(s_dim_lb, srv_config_get_int(CFG_DIM_EN, CFG_DEF_DIM_EN) != 0);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d 秒",
             (int)srv_config_get_int(CFG_DIM_AFTER, CFG_DEF_DIM_AFTER));
    lv_label_set_text(s_time_lb, buf);
}

static void on_dim_toggle(lv_event_t *e)
{
    (void)e;

    bool on = srv_config_get_int(CFG_DIM_EN, CFG_DEF_DIM_EN) == 0;
    srv_config_set_int(CFG_DIM_EN, on ? 1 : 0, true);
    bright_refresh();
}

static void on_time_step(lv_event_t *e)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(e);

    /* 越界由 srv_config 按表里的 min/max 夹住（10~600 秒） */
    int v = (int)srv_config_get_int(CFG_DIM_AFTER, CFG_DEF_DIM_AFTER) + delta;
    srv_config_set_int(CFG_DIM_AFTER, v, true);
    bright_refresh();
}

void ui_set_bright_create(lv_obj_t *root)
{
    ui_titlebar_create(root, "亮度", app_navigator_back_cb, NULL);
    lv_obj_t *list = ui_settings_list_create(root);

    lv_obj_t *row = ui_row_create(list, UI_ROW_Y(0), &ic_brightness, "自动降低亮度");
    s_dim_lb = ui_row_switch_create(row, true, on_dim_toggle, NULL);

    row = ui_row_create(list, UI_ROW_Y(1), NULL, "闲置多久后降低");
    s_time_lb = ui_row_value_create(row, -46, 46);
    ui_row_btn_create(row, -8, 34, "+", on_time_step, (void *)(intptr_t)DIM_STEP);
    ui_row_btn_create(row, -94, 34, "-", on_time_step, (void *)(intptr_t)-DIM_STEP);

    bright_refresh();
}