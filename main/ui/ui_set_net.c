#include "ui_set_pages.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "ui_dialog.h"
#include "app_navigator.h"
#include "srv_config.h"
#include "srv_wifi.h"
#include "srv_ntp.h"

#include <stdio.h>
#include <string.h>

/* 网络历史最多 8 条，每行存一份 SSID 备份：
 * 点击时按 SSID 去查密码，而不是按行号取历史 —— 连接成功后历史顺序会变，
 * 按行号取会连到别的网络上去。 */
#define HIST_MAX    SRV_WIFI_HISTORY_MAX

static lv_obj_t *s_list;                      /*!< 列表容器（页面销毁时置 NULL） */
static lv_obj_t *s_power_lb;                  /*!< 网络开关状态 */
static lv_obj_t *s_cur_lb;                    /*!< 当前网络 */
static char      s_hist_ssid[HIST_MAX][SRV_WIFI_SSID_MAX];
static int       s_hist_count;

/* ===================== 状态刷新 ===================== */

static void net_refresh(void)
{
    bool en = srv_wifi_is_enabled();

    if (s_power_lb != NULL) {
        lv_label_set_text(s_power_lb, en ? "开" : "关");
    }

    if (s_cur_lb == NULL) {
        return;
    }

    if (!en) {
        lv_label_set_text(s_cur_lb, "已关闭");
    } else if (srv_wifi_is_connected()) {
        const char *ssid = srv_wifi_get_ssid();
        lv_label_set_text(s_cur_lb, (ssid != NULL && ssid[0] != '\0') ? ssid : "已连接");
    } else if (srv_wifi_is_connecting()) {
        /* 连上要几秒，中间显示"连接中…"，不然看着像没反应 */
        lv_label_set_text(s_cur_lb, "连接中…");
    } else {
        lv_label_set_text(s_cur_lb, "未连接");
    }
}

static void net_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    net_refresh();
}

static void net_root_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    lv_timer_t *t = (lv_timer_t *)lv_event_get_user_data(e);
    if (t != NULL) {
        lv_timer_delete(t);
    }
}

static void list_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    /* 手快「返回又进来」时新页面已经建好了，旧页面的回调不能把新指针清掉 */
    if (lv_event_get_target(e) != s_list) {
        return;
    }
    s_list = NULL;
}

/* ===================== 交互 ===================== */

static void list_rebuild(void);       /* 删历史后要重建列表，先声明 */

static void on_power(lv_event_t *e)
{
    (void)e;

    bool on = !srv_wifi_is_enabled();
    srv_config_set_int(CFG_WIFI_EN, on ? 1 : 0, true);
    srv_wifi_set_enabled(on);

    if (on) {
        srv_ntp_autostart();        /* 联网后顺带校时（NTP 开关另在「时间」页） */
    }
    net_refresh();
}

static void on_reconnect(lv_event_t *e)
{
    (void)e;
    if (srv_wifi_is_enabled()) {
        srv_wifi_autoconnect();
    }
    net_refresh();
}

static void on_add_net(lv_event_t *e)
{
    (void)e;
    app_navigator_switch(PAGE_SET_NET_ADD, true);
}

/* 点一条历史直接连（manual = true：让后台自动连接别插嘴） */
static void on_hist(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_hist_count) {
        return;
    }

    char pass[SRV_WIFI_PASS_MAX] = {0};
    if (srv_wifi_history_get(s_hist_ssid[idx], pass, sizeof(pass)) != ESP_OK) {
        return;
    }

    /* save = false：历史里已经有了，不必再写一遍 */
    srv_wifi_connect(s_hist_ssid[idx], pass, false, true);

    if (s_cur_lb != NULL) {
        lv_label_set_text(s_cur_lb, "连接中…");
    }
}

/* 长按历史行：删掉这条记录（只删记录，不断开当前连接） */
static void on_hist_del_ok(void *arg)
{
    const char *ssid = (const char *)arg;
    srv_wifi_history_remove(ssid);
    list_rebuild();
}

static void on_hist_long(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_hist_count) {
        return;
    }

    ui_dialog_confirm("删除网络记录", s_hist_ssid[idx], "删除", on_hist_del_ok,
                      s_hist_ssid[idx]);
}

/* ===================== 列表 ===================== */

static void build_list(lv_obj_t *list)
{
    /* 网络开关 */
    lv_obj_t *row = ui_row_create(list, UI_ROW_Y(0), &ic_wifi, "网络");
    s_power_lb = ui_row_btn_create(row, -8, 52, "关", on_power, NULL);

    /* 当前网络：点击重新匹配历史连接 */
    row = ui_row_create(list, UI_ROW_Y(1), NULL, "当前网络");
    s_cur_lb = ui_row_btn_create(row, -8, 130, "--", on_reconnect, NULL);
    /* SSID 最长 32 字符，放不下就循环滚动，别把按钮撑破 */
    lv_obj_set_width(s_cur_lb, 118);
    lv_label_set_long_mode(s_cur_lb, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(s_cur_lb, 6000, 0);

    /* 添加网络：进二级页扫描附近热点，选一个连（加密的要输密码） */
    row = ui_row_create(list, UI_ROW_Y(2), &ic_search, "添加网络");
    ui_add_click_feedback(row);
    lv_obj_add_event_cb(row, on_add_net, LV_EVENT_CLICKED, NULL);

    lv_obj_t *arrow = lv_image_create(row);
    lv_image_set_src(arrow, &ic_arrow_right);
    lv_obj_set_style_image_recolor(arrow, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_radius(arrow, 0, 0);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -10, 0);

    /* 网络历史：点一条直接连，长按删除 */
    lv_obj_t *sec = lv_label_create(list);
    lv_obj_set_style_text_font(sec, &font_ui_14, 0);
    lv_obj_set_style_text_color(sec, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(sec, "网络历史（长按删除）");
    lv_obj_set_pos(sec, UI_ROW_X + 4, UI_ROW_Y(3) + 10);

    s_hist_count = srv_wifi_history_count();
    if (s_hist_count > HIST_MAX) {
        s_hist_count = HIST_MAX;
    }

    if (s_hist_count == 0) {
        lv_obj_t *lb = lv_label_create(list);
        lv_obj_set_style_text_font(lb, &font_ui_14, 0);
        lv_obj_set_style_text_color(lb, UI_COLOR_TEXT_DIM, 0);
        lv_label_set_text(lb, "（暂无记录）");
        lv_obj_set_pos(lb, UI_ROW_X + 4, UI_ROW_Y(4) + 10);
    }

    for (int i = 0; i < s_hist_count; i++) {
        char ssid[SRV_WIFI_SSID_MAX] = {0};
        char pass[SRV_WIFI_PASS_MAX] = {0};
        if (srv_wifi_history_at(i, ssid, sizeof(ssid), pass, sizeof(pass)) != ESP_OK) {
            continue;
        }
        /* 用 snprintf 而不是 strncpy：SSID 正好 32 字符时 strncpy 会触发
         * -Werror=stringop-truncation（IDF 把告警当错误） */
        snprintf(s_hist_ssid[i], sizeof(s_hist_ssid[i]), "%s", ssid);

        lv_obj_t *r = ui_row_create(list, UI_ROW_Y(4 + i), NULL, ssid);
        ui_add_click_feedback(r);
        lv_obj_add_event_cb(r, on_hist_long, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
        ui_row_btn_create(r, -8, 64, "连接", on_hist, (void *)(intptr_t)i);
    }
}

/**
 * @brief 重建列表内容
 *
 * 删掉一条历史后行号和行位置都要重排，整块重建最省事。清空前必须先把
 * 指向列表里控件的 static 清掉，否则它们就是野指针（net_refresh 会往里写）。
 */
static void list_rebuild(void)
{
    if (s_list == NULL) {
        return;
    }
    s_power_lb = NULL;
    s_cur_lb   = NULL;
    lv_obj_clean(s_list);
    build_list(s_list);
    net_refresh();
}

/* ===================== 页面 ===================== */

void ui_set_net_create(lv_obj_t *root)
{
    ui_titlebar_create(root, "网络", app_navigator_back_cb, NULL);

    s_list = ui_settings_list_create(root);
    lv_obj_add_event_cb(s_list, list_del_cb, LV_EVENT_DELETE, NULL);
    build_list(s_list);

    net_refresh();
    lv_timer_t *tmr = lv_timer_create(net_tick_cb, 1000, NULL);
    lv_obj_add_event_cb(root, net_root_del_cb, LV_EVENT_DELETE, tmr);
}
