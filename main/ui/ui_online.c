#include "ui_online.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "app_navigator.h"

#include "srv_catalog.h"
#include "srv_server.h"
#include "srv_wifi.h"

#include "bsp/esp32_c5_touch_lcd_2.8.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ui_online";

static lv_obj_t *s_root;
static lv_obj_t *s_addr_lb;
static lv_obj_t *s_hint_lb;
static lv_obj_t *s_list;

/* ===================== 刷新 ===================== */

static void refresh_addr_locked(void)
{
    const char *url = srv_server_get_url();
    if (url != NULL && url[0] != '\0') {
        lv_label_set_text(s_addr_lb, url);
        lv_obj_set_style_text_color(s_addr_lb, UI_COLOR_TEXT, 0);
    } else {
        lv_label_set_text(s_addr_lb, "未配置");
        lv_obj_set_style_text_color(s_addr_lb, UI_COLOR_TEMP, 0);
    }
}

/* ===================== 发现 ===================== */

static void discover_task(void *arg)
{
    (void)arg;

    esp_err_t err = srv_server_discover(3000);

    if (bsp_display_lock(1000) == ESP_OK) {
        if (s_root != NULL) {
            refresh_addr_locked();
            lv_label_set_text(s_hint_lb,
                              err == ESP_OK ? "已发现服务端" :
                              err == ESP_ERR_TIMEOUT ? "没找到服务端（同网段？服务端在跑？）" :
                              "发现失败");
            lv_obj_set_style_text_color(s_hint_lb,
                                        err == ESP_OK ? UI_COLOR_OK : UI_COLOR_TEMP, 0);
        }
        bsp_display_unlock();
    }
    vTaskDelete(NULL);
}

static void on_discover(lv_event_t *e)
{
    (void)e;
    if (!srv_wifi_is_connected()) {
        lv_label_set_text(s_hint_lb, "先连上 WiFi");
        lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_TEMP, 0);
        return;
    }
    lv_label_set_text(s_hint_lb, "正在发现 ...");
    lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_TEXT_DIM, 0);

    if (srv_server_auto_discover() != ESP_OK) {
        lv_label_set_text(s_hint_lb, "发现任务创建失败");
        lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_TEMP, 0);
    }
}

/* ===================== 书单 ===================== */

static void add_book_row(const srv_catalog_book_t *b)
{
    lv_obj_t *row = ui_row_create(s_list, (lv_coord_t)(lv_obj_get_child_count(s_list) * 44),
                                  &ic_book_open, b->title);
    if (row == NULL) {
        return;
    }

    /* 右侧：章数 + 大小（太小放不下就只显示章数） */
    char info[32];
    snprintf(info, sizeof(info), "%d 章", b->chapters);
    lv_obj_t *val = ui_row_value_create(row, -12, 90);
    lv_label_set_text(val, info);
    lv_obj_set_style_text_color(val, UI_COLOR_TEXT_DIM, 0);
}

static void fetch_task(void *arg)
{
    (void)arg;

    srv_catalog_book_t *list = NULL;
    int count = 0;
    esp_err_t err = srv_catalog_fetch(&list, &count);

    if (bsp_display_lock(1000) == ESP_OK) {
        if (s_root != NULL) {
            lv_obj_clean(s_list);

            if (err == ESP_ERR_INVALID_STATE) {
                lv_label_set_text(s_hint_lb, "先发现或填写服务端地址");
                lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_TEMP, 0);
            } else if (err != ESP_OK) {
                lv_label_set_text(s_hint_lb, "拉取书单失败（服务端没开？地址不对？）");
                lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_TEMP, 0);
            } else if (count == 0) {
                lv_label_set_text(s_hint_lb, "服务端书库是空的");
                lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_TEXT_DIM, 0);
            } else {
                for (int i = 0; i < count; i++) {
                    add_book_row(&list[i]);
                }
                char msg[48];
                snprintf(msg, sizeof(msg), "共 %d 本（下载功能开发中）", count);
                lv_label_set_text(s_hint_lb, msg);
                lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_OK, 0);
            }
        }
        bsp_display_unlock();
    }

    srv_catalog_free(list);
    vTaskDelete(NULL);
}

static void on_fetch(lv_event_t *e)
{
    (void)e;
    if (srv_server_get_url()[0] == '\0') {
        lv_label_set_text(s_hint_lb, "先发现或填写服务端地址");
        lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_TEMP, 0);
        return;
    }
    lv_label_set_text(s_hint_lb, "正在拉取书单 ...");
    lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_TEXT_DIM, 0);

    if (xTaskCreate(fetch_task, "ui_fetch", 6144, NULL, 3, NULL) != pdPASS) {
        lv_label_set_text(s_hint_lb, "任务创建失败");
        lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_TEMP, 0);
    }
}

/* ===================== 页面 ===================== */

static void online_root_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    if (lv_event_get_target(e) != s_root) {
        return;
    }
    s_root     = NULL;
    s_addr_lb  = NULL;
    s_hint_lb  = NULL;
    s_list     = NULL;
}

void ui_online_create(lv_obj_t *root)
{
    s_root = root;

    ui_titlebar_create(root, "在线书库", app_navigator_back_cb, NULL);

    /* ---- 服务端地址 + 发现 ---- */
    lv_obj_t *row = ui_row_create(root, 4, &ic_wifi, "服务端");
    s_addr_lb = ui_row_value_create(row, -12, 150);
    lv_obj_set_style_text_color(s_addr_lb, UI_COLOR_TEXT_DIM, 0);
    refresh_addr_locked();

    row = ui_row_create(root, 48, NULL, "搜索服务端");
    ui_row_btn_create(row, -8, 64, "发现", on_discover, NULL);

    row = ui_row_create(root, 92, NULL, "拉取书单");
    ui_row_btn_create(row, -8, 64, "刷新", on_fetch, NULL);

    s_hint_lb = lv_label_create(root);
    lv_obj_set_style_text_font(s_hint_lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_long_mode(s_hint_lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_hint_lb, ui_scr_w() - 20);
    lv_label_set_text(s_hint_lb, "点「发现」在同一 WiFi 下搜索，或去设置里填地址");
    lv_obj_set_pos(s_hint_lb, 10, 136);

    /* ---- 书单 ---- */
    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(list, ui_scr_w(), ui_scr_h() - 160);
    lv_obj_set_pos(list, 0, 160);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    s_list = list;

    lv_obj_add_event_cb(root, online_root_del_cb, LV_EVENT_DELETE, NULL);
}
