#include "ui_set_pages.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "app_navigator.h"
#include "srv_config.h"
#include "srv_sensors.h"

/*! 温湿度记录采样间隔：沿用原来的 10 分钟。 */
#define TEMP_REC_INTERVAL_MIN  10

static lv_obj_t *s_rec_sw;

static void record_refresh(void)
{
    ui_row_switch_set(s_rec_sw, srv_config_get_int(CFG_TEMP_REC_EN, 1) != 0);
}

static void on_temp_rec_toggle(lv_event_t *e)
{
    (void)e;

    bool on = srv_config_get_int(CFG_TEMP_REC_EN, 1) == 0;
    srv_config_set_int(CFG_TEMP_REC_EN, on ? 1 : 0, true);
    if (on) {
        srv_sensors_record_start(TEMP_REC_INTERVAL_MIN);
    } else {
        srv_sensors_record_stop();
    }
    record_refresh();
}

void ui_set_record_create(lv_obj_t *root)
{
    ui_titlebar_create(root, "记录", app_navigator_back_cb, NULL);
    lv_obj_t *list = ui_settings_list_create(root);

    lv_obj_t *row = ui_row_create(list, UI_ROW_Y(0), &ic_page, "温湿度记录");
    s_rec_sw = ui_row_switch_create(row,
                                    srv_config_get_int(CFG_TEMP_REC_EN, 1) != 0,
                                    on_temp_rec_toggle, NULL);
    record_refresh();
}
