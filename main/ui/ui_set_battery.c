/**
 * @file ui_set_battery.c
 * @brief 设置 -> 电池：电量 / 电压 / ADC 原始值
 *
 * 三样都来自 srv_battery（真机上读的是 CH32V003 的 10 位 ADC，板上 3:1 分压）。
 * ADC 原始值平时没什么用，但它能一眼看出「电压换算对不对」—— 标定时会看它。
 */
#include "ui_set_pages.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "ui_dialog.h"
#include "app_navigator.h"

#include "srv_battery.h"
#include "srv_config.h"
#include "esp_log.h"

#include <stdio.h>

static const char *TAG = "ui_battery";

static lv_obj_t *s_root;
static lv_obj_t *s_pct_lb;      /*!< 大号百分比数字 */
static lv_obj_t *s_bar;
static lv_obj_t *s_volt_lb;
static lv_obj_t *s_adc_lb;
static lv_obj_t *s_cal_lb;      /*!< 校准状态：未校准 / +5.0% */

static void bat_refresh(lv_timer_t *timer)
{
    (void)timer;

    char buf[32];

    float vol = 0.0f;
    int   pct = 0;
    if (srv_battery_get(&vol, &pct) == ESP_OK) {
        snprintf(buf, sizeof(buf), "%d", pct);
        lv_label_set_text(s_pct_lb, buf);
        lv_bar_set_value(s_bar, pct, LV_ANIM_OFF);

        snprintf(buf, sizeof(buf), "%.2f V", (double)vol);
        lv_label_set_text(s_volt_lb, buf);
    } else {
        lv_label_set_text(s_pct_lb, "--");
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
        lv_label_set_text(s_volt_lb, "--");
    }

    uint16_t adc = 0;
    if (srv_battery_get_adc(&adc) == ESP_OK) {
        snprintf(buf, sizeof(buf), "%u", (unsigned)adc);
        lv_label_set_text(s_adc_lb, buf);
    } else {
        lv_label_set_text(s_adc_lb, "--");
    }

    const int k = srv_battery_cal_k();
    if (k == CFG_DEF_BAT_CAL_K) {
        lv_label_set_text(s_cal_lb, "未校准");
        lv_obj_set_style_text_color(s_cal_lb, UI_COLOR_TEXT_DIM, 0);
        return;
    }
    /* 系数换算成百分比偏差，比千分比直观（1025‰ → +2.5%） */
    snprintf(buf, sizeof(buf), "%+.1f%%", (double)(k - CFG_DEF_BAT_CAL_K) / 10.0);
    lv_label_set_text(s_cal_lb, buf);
    lv_obj_set_style_text_color(s_cal_lb, UI_COLOR_OK, 0);
}

/* ===================== 校准 ===================== */

static void cal_ask_measured(void);

static void on_cal_menu(int idx, void *arg)
{
    (void)arg;

    if (idx == 0) {
        cal_ask_measured();
    } else if (idx == 1) {
        srv_battery_cal_reset();
        bat_refresh(NULL);
        ui_alert_show("已恢复默认", UI_COLOR_OK, "电压按板上 3:1 分压原样换算",
                      "知道了", NULL, NULL, 2000);
    }
}

static void on_measured_done(const int *v, void *arg)
{
    (void)arg;

    const float measured = (float)v[0] + (float)v[1] / 100.0f;
    esp_err_t ret = srv_battery_calibrate(measured);

    bat_refresh(NULL);

    if (ret == ESP_OK) {
        char note[48];
        snprintf(note, sizeof(note), "实测 %.2fV，之后所有读数按它折算", (double)measured);
        ui_alert_show("校准完成", UI_COLOR_OK, note, "知道了", NULL, NULL, 2400);
    } else if (ret == ESP_ERR_INVALID_STATE) {
        ui_alert_show("校准不了", UI_COLOR_WARN, "没装电池（ADC 读 0），先插上电池",
                      "知道了", NULL, NULL, 2400);
    } else {
        ui_alert_show("校准失败", UI_COLOR_WARN, esp_err_to_name(ret),
                      "知道了", NULL, NULL, 2200);
    }
}

static void cal_ask_measured(void)
{
    /* 两列滚轮：整数位 + 两位小数。初值取当前读数，四舍五入到 0.01V */
    float vol = 3.80f;
    int   pct = 0;
    if (srv_battery_get(&vol, &pct) != ESP_OK) {
        vol = 3.80f;
    }

    int whole = (int)vol;
    int frac  = (int)((vol - (float)whole) * 100.0f + 0.5f);
    if (frac >= 100) {
        whole++;
        frac = 0;
    }
    if (whole < 2) {                    /* 滚轮范围 2~4V，超出就往里夹 */
        whole = 2;
    }
    if (whole > 4) {
        whole = 4;
    }

    const ui_picker_col_t cols[2] = {
        { "V",     2, 3,   whole },     /* 2 / 3 / 4 */
        { ".01V",  0, 100, frac  },     /* .00 ~ .99 */
    };
    ui_picker_show("输入万用表实测电压", 2, cols, on_measured_done, NULL);
}

static void on_cal_click(lv_event_t *e)
{
    (void)e;

    static const ui_menu_item_t items[] = {
        {"输入实测电压", &ic_calibration},
        {"恢复默认",     &ic_reset},
    };
    ui_menu_show("电池校准", items, 2, on_cal_menu, NULL);
}

static void bat_root_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    /* 定时器句柄走事件 user_data：static 会被新页面覆盖（见 ui_clock 的说明） */
    lv_timer_t *t = (lv_timer_t *)lv_event_get_user_data(e);
    if (t != NULL) {
        lv_timer_delete(t);
    }

    if (lv_event_get_target(e) != s_root) {
        return;
    }
    s_root    = NULL;
    s_pct_lb  = NULL;
    s_bar     = NULL;
    s_volt_lb = NULL;
    s_adc_lb  = NULL;
    s_cal_lb  = NULL;
}

void ui_set_battery_create(lv_obj_t *root)
{
    s_root = root;

    ui_titlebar_create(root, "电池", app_navigator_back_cb, NULL);

    /* ---- 大号百分比（数字用时钟字体，% 单独用小字，跟时钟页的秒一个套路） ---- */
    s_pct_lb = lv_label_create(root);
    lv_obj_set_style_text_color(s_pct_lb, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_pct_lb, &font_clock_48, 0);
    lv_label_set_text(s_pct_lb, "--");
    lv_obj_align(s_pct_lb, LV_ALIGN_TOP_MID, -6, 34);

    lv_obj_t *unit = lv_label_create(root);
    lv_obj_set_style_text_color(unit, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(unit, &font_ui_14, 0);
    lv_label_set_text(unit, "%");
    lv_obj_align_to(unit, s_pct_lb, LV_ALIGN_OUT_RIGHT_BOTTOM, 2, -6);

    /* ---- 电量条 ---- */
    s_bar = lv_bar_create(root);
    lv_obj_set_size(s_bar, 304, 10);
    lv_obj_set_pos(s_bar, 8, 96);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, UI_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, UI_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 5, LV_PART_INDICATOR);

    /* ---- 明细 ---- */
    lv_obj_t *row = ui_row_create(root, 104, &ic_charging, "电压");
    s_volt_lb = ui_row_value_create(row, -12, 90);

    row = ui_row_create(root, 148, &ic_calibration, "ADC 原始值");
    s_adc_lb = ui_row_value_create(row, -12, 90);

    /* 校准：右侧显示当前状态（未校准 / +2.5%），点开菜单输入实测电压 */
    row = ui_row_create(root, 192, &ic_calibration, "校准");
    ui_add_click_feedback(row);
    lv_obj_add_event_cb(row, on_cal_click, LV_EVENT_CLICKED, NULL);
    s_cal_lb = ui_row_value_create(row, -12, 100);
    lv_obj_set_style_text_color(s_cal_lb, UI_COLOR_TEXT_DIM, 0);

    /* 每秒刷一次（读数由电池服务直接换算，不再做平滑） */
    lv_timer_t *tmr = lv_timer_create(bat_refresh, 1000, NULL);
    lv_obj_add_event_cb(root, bat_root_del_cb, LV_EVENT_DELETE, tmr);
    bat_refresh(NULL);

    ESP_LOGI(TAG, "电池页就绪（校准系数 %d‰）", srv_battery_cal_k());
}