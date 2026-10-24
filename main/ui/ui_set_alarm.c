#include "ui_set_pages.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "ui_dialog.h"
#include "app_navigator.h"
#include "srv_config.h"

#include <stdio.h>

#define ALARM_NUM   SRV_CONFIG_ALARM_NUM
#define VOL_STEP    10

/* 键名与标题都用常量表：用 snprintf 拼 "alarm%d_h" 时编译器无法证明
 * i+1 只有一位数，会以 -Werror=format-truncation 直接报错 */
static const char *const K_EN[ALARM_NUM]   = {"alarm1_en", "alarm2_en", "alarm3_en"};
static const char *const K_H[ALARM_NUM]    = {"alarm1_h",  "alarm2_h",  "alarm3_h"};
static const char *const K_M[ALARM_NUM]    = {"alarm1_m",  "alarm2_m",  "alarm3_m"};
static const char *const NAME[ALARM_NUM]   = {"闹钟 1", "闹钟 2", "闹钟 3"};

static lv_obj_t *s_vol_lb;
static lv_obj_t *s_time_lb[ALARM_NUM];
static lv_obj_t *s_sw_lb[ALARM_NUM];

static void alarm_refresh(void)
{
    char buf[24];

    snprintf(buf, sizeof(buf), "%d%%", (int)srv_config_get_int(CFG_ALARM_VOLUME, CFG_DEF_ALARM_VOLUME));
    lv_label_set_text(s_vol_lb, buf);

    for (int i = 0; i < ALARM_NUM; i++) {
        int h = (int)srv_config_get_int(K_H[i], 0);
        int m = (int)srv_config_get_int(K_M[i], 0);
        snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
        lv_label_set_text(s_time_lb[i], buf);

        ui_row_switch_set(s_sw_lb[i], srv_config_get_int(K_EN[i], 0) != 0);
    }
}

/* ===================== 交互 ===================== */

static void on_vol_step(lv_event_t *e)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(e);

    int v = (int)srv_config_get_int(CFG_ALARM_VOLUME, CFG_DEF_ALARM_VOLUME) + delta;
    if (v < 0)   v = 0;
    if (v > 100) v = 100;

    srv_config_set_int(CFG_ALARM_VOLUME, v, true);
    alarm_refresh();
}

static void on_time_done(const int *v, void *arg)
{
    int i = (int)(intptr_t)arg;

    srv_config_set_int(K_H[i], v[0], false);
    srv_config_set_int(K_M[i], v[1], true);

    alarm_refresh();
}

static void on_time_click(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);

    const ui_picker_col_t cols[2] = {
        {"时", 0, 24, (int)srv_config_get_int(K_H[i], 0)},
        {"分", 0, 60, (int)srv_config_get_int(K_M[i], 0)},
    };
    ui_picker_show(NAME[i], 2, cols, on_time_done, (void *)(intptr_t)i);
}

static void on_sw_click(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);

    bool on = srv_config_get_int(K_EN[i], 0) == 0;
    srv_config_set_int(K_EN[i], on ? 1 : 0, true);

    alarm_refresh();
}

/* ===================== 页面 ===================== */

void ui_set_alarm_create(lv_obj_t *root)
{
    ui_titlebar_create(root, "闹钟", app_navigator_back_cb, NULL);
    lv_obj_t *list = ui_settings_list_create(root);

    /* 音量：行内 ± ，步长 10（0~100 用滚轮要拨太多下） */
    lv_obj_t *row = ui_row_create(list, UI_ROW_Y(0), &ic_music, "闹钟音量");
    s_vol_lb = ui_row_value_create(row, -46, 46);
    ui_row_btn_create(row, -8, 34, "+", on_vol_step, (void *)(intptr_t)VOL_STEP);
    ui_row_btn_create(row, -94, 34, "-", on_vol_step, (void *)(intptr_t)-VOL_STEP);

    for (int i = 0; i < ALARM_NUM; i++) {
        row = ui_row_create(list, UI_ROW_Y(1 + i), &ic_timer, NAME[i]);
        s_time_lb[i] = ui_row_btn_create(row, -62, 74, "--:--",
                                         on_time_click, (void *)(intptr_t)i);
        s_sw_lb[i] = ui_row_switch_create(row, false, on_sw_click, (void *)(intptr_t)i);
    }

    alarm_refresh();
}
