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
#include "srv_hist.h"
#include "esp_log.h"

#include <stdio.h>

static const char *TAG = "ui_battery";

static lv_obj_t *s_root;
static lv_obj_t *s_pct_lb;      /*!< 大号百分比数字 */
static lv_obj_t *s_bar;
static lv_obj_t *s_volt_lb;
static lv_obj_t *s_adc_lb;
static lv_obj_t *s_cal_lb;      /*!< 校准状态：未校准 / +5.0% */
static lv_obj_t *s_chart;       /*!< 电量曲线 */
static lv_chart_series_t *s_series;
static uint32_t s_last_hist_seq;/*!< 上次画曲线时的采样序号 */

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
    } else {
        /* 系数换算成百分比偏差，比千分比直观（1025‰ → +2.5%） */
        snprintf(buf, sizeof(buf), "%+.1f%%", (double)(k - CFG_DEF_BAT_CAL_K) / 10.0);
        lv_label_set_text(s_cal_lb, buf);
        lv_obj_set_style_text_color(s_cal_lb, UI_COLOR_OK, 0);
    }

    /* 曲线只在有新采样点时重画（60 秒一次），没必要每秒刷 */
    const uint32_t seq = srv_hist_seq();
    if (seq != s_last_hist_seq) {
        s_last_hist_seq = seq;

        srv_hist_point_t pts[SRV_HIST_POINTS];
        int32_t vals[SRV_HIST_POINTS];
        const int n = srv_hist_get(pts, SRV_HIST_POINTS);
        for (int i = 0; i < n; i++) {
            vals[i] = (pts[i].battery_pct >= 0) ? pts[i].battery_pct
                                                : LV_CHART_POINT_NONE;
        }
        if (n > 0) {
            lv_chart_set_series_values(s_chart, s_series, vals, (uint32_t)n);
            lv_chart_refresh(s_chart);
        }
    }
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
    s_chart   = NULL;
    s_series  = NULL;
}

void ui_set_battery_create(lv_obj_t *root)
{
    s_root = root;

    ui_titlebar_create(root, "电池", app_navigator_back_cb, NULL);

    /* 明细 + 曲线加起来比一屏高（横屏只有 212px 可用），统一放进可滚动容器。
     * 标题栏留在 root 上不跟着滚，所以 y 坐标都相对 body（减掉标题栏高度）。 */
    lv_obj_t *body = lv_obj_create(root);
    lv_obj_set_size(body, ui_scr_w(), ui_scr_h() - UI_TITLEBAR_H);
    lv_obj_set_pos(body, 0, UI_TITLEBAR_H);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);

    /* ---- 大号百分比（数字用时钟字体，% 单独用小字，跟时钟页的秒一个套路） ---- */
    s_pct_lb = lv_label_create(body);
    lv_obj_set_style_text_color(s_pct_lb, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_pct_lb, &font_clock_48, 0);
    lv_label_set_text(s_pct_lb, "--");
    lv_obj_align(s_pct_lb, LV_ALIGN_TOP_MID, -6, 6);

    lv_obj_t *unit = lv_label_create(body);
    lv_obj_set_style_text_color(unit, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(unit, &font_ui_14, 0);
    lv_label_set_text(unit, "%");
    lv_obj_align_to(unit, s_pct_lb, LV_ALIGN_OUT_RIGHT_BOTTOM, 2, -6);

    /* ---- 电量条 ---- */
    s_bar = lv_bar_create(body);
    lv_obj_set_size(s_bar, UI_ROW_W, 10);
    lv_obj_set_pos(s_bar, 8, 68);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, UI_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, UI_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 5, LV_PART_INDICATOR);

    /* ---- 明细 ---- */
    lv_obj_t *row = ui_row_create(body, 76, &ic_charging, "电压");
    s_volt_lb = ui_row_value_create(row, -12, 90);

    row = ui_row_create(body, 120, &ic_calibration, "ADC 原始值");
    s_adc_lb = ui_row_value_create(row, -12, 90);

    /* 校准：右侧显示当前状态（未校准 / +2.5%），点开菜单输入实测电压 */
    row = ui_row_create(body, 164, &ic_calibration, "校准");
    ui_add_click_feedback(row);
    lv_obj_add_event_cb(row, on_cal_click, LV_EVENT_CLICKED, NULL);
    s_cal_lb = ui_row_value_create(row, -12, 100);
    lv_obj_set_style_text_color(s_cal_lb, UI_COLOR_TEXT_DIM, 0);

    /* ---- 电量曲线（运行时记录，60 秒一点） ----
     * 横屏要往上滚一点才看得到完整曲线，竖屏直接可见。 */
    lv_obj_t *cap = lv_label_create(body);
    lv_obj_set_style_text_color(cap, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(cap, &font_ui_14, 0);
    lv_label_set_text(cap, "最近电量变化");
    lv_obj_set_pos(cap, 10, 216);

    s_chart = lv_chart_create(body);
    lv_obj_set_size(s_chart, UI_ROW_W, 76);
    lv_obj_set_pos(s_chart, 8, 238);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_point_count(s_chart, SRV_HIST_POINTS);
    lv_chart_set_div_line_count(s_chart, 4, 0);
    lv_obj_set_style_bg_color(s_chart, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_radius(s_chart, 6, 0);
    lv_obj_set_style_pad_all(s_chart, 4, 0);
    s_series = lv_chart_add_series(s_chart, UI_COLOR_PRIMARY, LV_CHART_AXIS_PRIMARY_Y);

    /* 每秒刷一次（读数由电池服务直接换算，不再做平滑） */
    lv_timer_t *tmr = lv_timer_create(bat_refresh, 1000, NULL);
    lv_obj_add_event_cb(root, bat_root_del_cb, LV_EVENT_DELETE, tmr);
    bat_refresh(NULL);

    ESP_LOGI(TAG, "电池页就绪（校准系数 %d‰）", srv_battery_cal_k());
}