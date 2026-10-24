#include "ui_set_pages.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "ui_dialog.h"
#include "app_navigator.h"
#include "srv_config.h"

#include <stdio.h>

#define MUTE_NUM    SRV_CONFIG_MUTE_NUM

/* 同闹钟页：键名/标题走常量表，避免 snprintf 拼字符串触发
 * -Werror=format-truncation */
static const char *const K_EN[MUTE_NUM]    = {"mute1_en", "mute2_en", "mute3_en"};
static const char *const K_S[MUTE_NUM]     = {"mute1_s",  "mute2_s",  "mute3_s"};
static const char *const K_E[MUTE_NUM]     = {"mute1_e",  "mute2_e",  "mute3_e"};
static const char *const NAME[MUTE_NUM]    = {"时段 1", "时段 2", "时段 3"};
static const char *const TITLE[MUTE_NUM]   = {"时段 1 静音", "时段 2 静音", "时段 3 静音"};

static lv_obj_t *s_span_lb[MUTE_NUM];
static lv_obj_t *s_sw_lb[MUTE_NUM];

/* 配置里时段存的是「当天第几分钟」，界面上按 HH:MM 显示 */
static void min_to_hm(int total, int *h, int *m)
{
    if (total < 0)    total = 0;
    if (total > 1439) total = 1439;
    *h = total / 60;
    *m = total % 60;
}

static void music_refresh(void)
{
    for (int i = 0; i < MUTE_NUM; i++) {
        char buf[24];
        int  sh, sm, eh, em;

        min_to_hm((int)srv_config_get_int(K_S[i], 0), &sh, &sm);
        min_to_hm((int)srv_config_get_int(K_E[i], 0), &eh, &em);
        snprintf(buf, sizeof(buf), "%02d:%02d-%02d:%02d", sh, sm, eh, em);
        lv_label_set_text(s_span_lb[i], buf);

        ui_row_switch_set(s_sw_lb[i], srv_config_get_int(K_EN[i], 0) != 0);
    }
}

/* ===================== 交互 ===================== */

static void on_span_done(const int *v, void *arg)
{
    int i = (int)(intptr_t)arg;

    srv_config_set_int(K_S[i], v[0] * 60 + v[1], false);
    srv_config_set_int(K_E[i], v[2] * 60 + v[3], true);

    music_refresh();
}

static void on_span_click(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int sh, sm, eh, em;

    min_to_hm((int)srv_config_get_int(K_S[i], 0), &sh, &sm);
    min_to_hm((int)srv_config_get_int(K_E[i], 0), &eh, &em);

    const ui_picker_col_t cols[4] = {
        {"起时", 0, 24, sh},
        {"起分", 0, 60, sm},
        {"止时", 0, 24, eh},
        {"止分", 0, 60, em},
    };
    ui_picker_show(TITLE[i], 4, cols, on_span_done, (void *)(intptr_t)i);
}

static void on_sw_click(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);

    bool on = srv_config_get_int(K_EN[i], 0) == 0;
    srv_config_set_int(K_EN[i], on ? 1 : 0, true);

    music_refresh();
}

/* ===================== 页面 ===================== */

void ui_set_music_create(lv_obj_t *root)
{
    ui_titlebar_create(root, "音乐", app_navigator_back_cb, NULL);
    lv_obj_t *list = ui_settings_list_create(root);

    for (int i = 0; i < MUTE_NUM; i++) {
        lv_obj_t *row = ui_row_create(list, UI_ROW_Y(i), &ic_music, NAME[i]);
        s_span_lb[i] = ui_row_btn_create(row, -62, 106, "--:--",
                                         on_span_click, (void *)(intptr_t)i);
        s_sw_lb[i] = ui_row_switch_create(row, false, on_sw_click, (void *)(intptr_t)i);
    }

    music_refresh();
}
