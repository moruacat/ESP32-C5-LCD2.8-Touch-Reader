/**
 * @file ui_set_env.c
 * @brief 设置 -> 环境：温湿度实时值 + 运行时曲线
 *
 * 数据来自 srv_hist（60 秒一个点，只记录本次运行，不读历史 CSV）。
 * 曲线横轴是采样序号，纵轴：温度 -10~50 ℃、湿度 0~100 %。
 */
#include "ui_set_pages.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "ui_dialog.h"
#include "app_navigator.h"

#include "srv_sensors.h"
#include "srv_hist.h"

#include "esp_log.h"

#include <stdio.h>

static const char *TAG = "ui_env";

static lv_obj_t *s_root;
static lv_obj_t *s_temp_lb;
static lv_obj_t *s_humi_lb;
static lv_obj_t *s_temp_chart;
static lv_obj_t *s_humi_chart;
static lv_chart_series_t *s_temp_series;
static lv_chart_series_t *s_humi_series;
static uint32_t s_last_hist_seq;

static lv_obj_t *make_chart(lv_obj_t *parent, lv_coord_t y, const char *title,
                            int32_t min, int32_t max, lv_color_t color,
                            lv_chart_series_t **out_series)
{
    lv_obj_t *cap = lv_label_create(parent);
    lv_obj_set_style_text_color(cap, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(cap, &font_ui_14, 0);
    lv_label_set_text(cap, title);
    lv_obj_set_pos(cap, 10, y);

    lv_obj_t *chart = lv_chart_create(parent);
    lv_obj_set_size(chart, UI_ROW_W, 84);
    lv_obj_set_pos(chart, 8, y + 22);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, min, max);
    lv_chart_set_point_count(chart, SRV_HIST_POINTS);
    lv_chart_set_div_line_count(chart, 4, 0);
    lv_obj_set_style_bg_color(chart, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_radius(chart, 6, 0);
    lv_obj_set_style_pad_all(chart, 4, 0);
    *out_series = lv_chart_add_series(chart, color, LV_CHART_AXIS_PRIMARY_Y);
    return chart;
}

static void env_refresh(lv_timer_t *timer)
{
    (void)timer;

    char buf[32];

    float temp = 0.0f;
    float humi = 0.0f;
    if (srv_sensors_get_temp_humi(&temp, &humi) == ESP_OK) {
        snprintf(buf, sizeof(buf), "%.1f ℃", (double)temp);
        lv_label_set_text(s_temp_lb, buf);
        snprintf(buf, sizeof(buf), "%.1f %%", (double)humi);
        lv_label_set_text(s_humi_lb, buf);
    } else {
        lv_label_set_text(s_temp_lb, "--");
        lv_label_set_text(s_humi_lb, "--");
    }

    /* 只有新增采样点时才重画（60 秒一次） */
    const uint32_t seq = srv_hist_seq();
    if (seq == s_last_hist_seq) {
        return;
    }
    s_last_hist_seq = seq;

    srv_hist_point_t pts[SRV_HIST_POINTS];
    int32_t tvals[SRV_HIST_POINTS];
    int32_t hvals[SRV_HIST_POINTS];
    const int n = srv_hist_get(pts, SRV_HIST_POINTS);
    for (int i = 0; i < n; i++) {
        if (pts[i].temp_c10 == INT16_MIN) {
            tvals[i] = LV_CHART_POINT_NONE;
            hvals[i] = LV_CHART_POINT_NONE;
        } else {
            tvals[i] = pts[i].temp_c10;
            hvals[i] = (int32_t)pts[i].humi_p10;
        }
    }
    if (n > 0) {
        lv_chart_set_series_values(s_temp_chart, s_temp_series, tvals, (uint32_t)n);
        lv_chart_refresh(s_temp_chart);
        lv_chart_set_series_values(s_humi_chart, s_humi_series, hvals, (uint32_t)n);
        lv_chart_refresh(s_humi_chart);
    }
}

static void env_root_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    lv_timer_t *t = (lv_timer_t *)lv_event_get_user_data(e);
    if (t != NULL) {
        lv_timer_delete(t);
    }

    if (lv_event_get_target(e) != s_root) {
        return;
    }
    s_root        = NULL;
    s_temp_lb     = NULL;
    s_humi_lb     = NULL;
    s_temp_chart  = NULL;
    s_humi_chart  = NULL;
    s_temp_series = NULL;
    s_humi_series = NULL;
    s_last_hist_seq = 0;
}

void ui_set_env_create(lv_obj_t *root)
{
    s_root = root;

    ui_titlebar_create(root, "环境", app_navigator_back_cb, NULL);

    /* 实时值 + 两张曲线放不进一屏，放可滚动容器（与电池页同样的处理） */
    lv_obj_t *body = lv_obj_create(root);
    lv_obj_set_size(body, ui_scr_w(), ui_scr_h() - UI_TITLEBAR_H);
    lv_obj_set_pos(body, 0, UI_TITLEBAR_H);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);

    /* ---- 实时值 ---- */
    lv_obj_t *row = ui_row_create(body, 4, &ic_calibration, "温度");
    s_temp_lb = ui_row_value_create(row, -12, 100);
    lv_obj_set_style_text_color(s_temp_lb, UI_COLOR_TEXT, 0);

    row = ui_row_create(body, 48, &ic_calibration, "湿度");
    s_humi_lb = ui_row_value_create(row, -12, 100);
    lv_obj_set_style_text_color(s_humi_lb, UI_COLOR_TEXT, 0);

    /* ---- 曲线 ---- */
    s_temp_chart = make_chart(body, 96, "温度（℃）",
                              -100, 500, UI_COLOR_TEMP, &s_temp_series);
    s_humi_chart = make_chart(body, 214, "湿度（%）",
                              0, 1000, UI_COLOR_PRIMARY, &s_humi_series);

    lv_timer_t *tmr = lv_timer_create(env_refresh, 1000, NULL);
    lv_obj_add_event_cb(root, env_root_del_cb, LV_EVENT_DELETE, tmr);
    env_refresh(NULL);
}
