#include "ui_set_pages.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "ui_dialog.h"
#include "app_navigator.h"
#include "srv_config.h"
#include "srv_rtc.h"
#include "srv_ntp.h"

#include <stdio.h>

static lv_obj_t *s_ntp_lb;      /*!< NTP 开关状态 */
static lv_obj_t *s_tz_lb;       /*!< 时区显示 */
static lv_obj_t *s_off_lb;      /*!< 秒级微调显示 */
static lv_obj_t *s_manual_lb;   /*!< 手动调时按钮 */

static int days_in_month(int year, int month)
{
    static const int DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 31;
    }
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
        return 29;
    }
    return DAYS[month - 1];
}

static void time_refresh(void)
{
    bool ntp_on = srv_config_get_int(CFG_NTP_EN, 1) != 0;

    ui_row_switch_set(s_ntp_lb, ntp_on);

    char buf[16];

    /* 时区：按 UTC±H 显示 */
    int tz = (int)srv_config_get_int(CFG_NTP_TZ, CFG_DEF_NTP_TZ);
    snprintf(buf, sizeof(buf), "UTC%+d", tz);
    lv_label_set_text(s_tz_lb, buf);

    /* 偏移：与时分无关的纯秒级微调（校几秒的走时误差） */
    int fine = (int)srv_config_get_int(CFG_NTP_FINE, CFG_DEF_NTP_FINE);
    snprintf(buf, sizeof(buf), "%+d 秒", fine);
    lv_label_set_text(s_off_lb, buf);

    /* 手动调时：NTP 开着的时候不给改，否则下一次校时就被覆盖，
     * 看起来像"改了没生效"。 */
    srv_rtc_time_t t;
    if (srv_rtc_get_time(&t) == ESP_OK) {
        snprintf(buf, sizeof(buf), "%02d:%02d", t.hour, t.min);
    } else {
        snprintf(buf, sizeof(buf), "--:--");
    }
    lv_label_set_text(s_manual_lb, buf);
    lv_obj_set_style_text_color(s_manual_lb,
                                ntp_on ? UI_COLOR_TEXT_DIM : UI_COLOR_TEXT, 0);
}

/* ===================== 交互 ===================== */

static void on_ntp_toggle(lv_event_t *e)
{
    (void)e;

    bool on = srv_config_get_int(CFG_NTP_EN, 1) == 0;
    srv_config_set_int(CFG_NTP_EN, on ? 1 : 0, true);

    if (on) {
        srv_ntp_autostart();
    } else {
        srv_ntp_stop();
    }
    time_refresh();
}

/* ----- 时区（整小时，-12 ~ +14） ----- */

static void on_tz_done(const int *v, void *arg)
{
    (void)arg;

    srv_config_set_int(CFG_NTP_TZ, v[0], true);
    time_refresh();
}

static void on_tz_click(lv_event_t *e)
{
    (void)e;

    const ui_picker_col_t cols[1] = {
        {"小时", -12, 27, (int)srv_config_get_int(CFG_NTP_TZ, CFG_DEF_NTP_TZ)},
    };
    ui_picker_show("时区", 1, cols, on_tz_done, NULL);
}

/* ----- 偏移（秒级微调，-60 ~ +60 秒） ----- */

static void on_off_done(const int *v, void *arg)
{
    (void)arg;

    srv_config_set_int(CFG_NTP_FINE, v[0], true);
    time_refresh();
}

static void on_off_click(lv_event_t *e)
{
    (void)e;

    const ui_picker_col_t cols[1] = {
        {"秒", -60, 121, (int)srv_config_get_int(CFG_NTP_FINE, CFG_DEF_NTP_FINE)},
    };
    ui_picker_show("NTP 偏移", 1, cols, on_off_done, NULL);
}

/* ----- 手动调时 ----- */

static void on_manual_done(const int *v, void *arg)
{
    (void)arg;

    srv_rtc_time_t t = {
        .year  = v[0],
        .month = v[1],
        .day   = v[2],
        .hour  = v[3],
        .min   = v[4],
        .sec   = 0,
    };

    /* 日号夹到当月：滚轮里 31 号是常驻项，选到 2 月/4 月会越界 */
    int dim = days_in_month(t.year, t.month);
    if (t.day > dim) {
        t.day = dim;
    }
    t.dotw = srv_rtc_calc_dotw(t.year, t.month, t.day);

    srv_rtc_set_time(&t);
    time_refresh();
}

static void on_manual_click(lv_event_t *e)
{
    (void)e;

    if (srv_config_get_int(CFG_NTP_EN, 1) != 0) {
        return;
    }

    srv_rtc_time_t t;
    if (srv_rtc_get_time(&t) != ESP_OK) {
        return;
    }

    const ui_picker_col_t cols[5] = {
        {"年", 2024, 37, t.year},     /* 2024 ~ 2060 */
        {"月", 1,    12, t.month},
        {"日", 1,    31, t.day},
        {"时", 0,    24, t.hour},
        {"分", 0,    60, t.min},
    };
    ui_picker_show("手动调时", 5, cols, on_manual_done, NULL);
}

/* ===================== 页面 ===================== */

void ui_set_time_create(lv_obj_t *root)
{
    ui_titlebar_create(root, "时间", app_navigator_back_cb, NULL);
    lv_obj_t *list = ui_settings_list_create(root);

    lv_obj_t *row = ui_row_create(list, UI_ROW_Y(0), &ic_ntp, "NTP 校时");
    s_ntp_lb = ui_row_switch_create(row, true, on_ntp_toggle, NULL);

    row = ui_row_create(list, UI_ROW_Y(1), NULL, "时区");
    s_tz_lb = ui_row_btn_create(row, -8, 76, "UTC+8", on_tz_click, NULL);

    row = ui_row_create(list, UI_ROW_Y(2), NULL, "NTP 偏移");
    s_off_lb = ui_row_btn_create(row, -8, 76, "0 秒", on_off_click, NULL);

    row = ui_row_create(list, UI_ROW_Y(3), NULL, "手动调时");
    s_manual_lb = ui_row_btn_create(row, -8, 76, "--:--", on_manual_click, NULL);

    time_refresh();
}