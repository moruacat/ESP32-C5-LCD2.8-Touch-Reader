#include "ui_online.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "ui_dialog.h"
#include "app_navigator.h"

#include "srv_catalog.h"
#include "srv_server.h"
#include "srv_wifi.h"

#include "bsp/esp32_c5_touch_lcd_2.8.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 这个页面不打日志，没有 TAG */
static lv_obj_t *s_root;
static lv_obj_t *s_addr_lb;
static lv_obj_t *s_hint_lb;
static lv_obj_t *s_list;

/* 书单缓存：点击行要拿到对应的书，页面销毁时释放 */
static srv_catalog_book_t *s_books;
static int                 s_book_count;
static volatile bool       s_downloading;

/* 前置声明：发现任务要用到书单相关函数（实现在文件后半段） */
static void add_book_row(int idx);
static void discover_task(void *arg);

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

    /* 不能走 srv_server_auto_discover()：那只更新服务层状态，没有回调，
     * 页面会一直停在「正在发现」。这里自己起任务，发现成功顺带拉书单。 */
    if (xTaskCreate(discover_task, "ui_disc", 6144, NULL, 3, NULL) != pdPASS) {
        lv_label_set_text(s_hint_lb, "发现任务创建失败");
        lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_TEMP, 0);
    }
}

/* 发现 + 拉书单一条龙；完成后统一回写 UI */
static void discover_task(void *arg)
{
    (void)arg;

    esp_err_t err = srv_server_discover(3000);

    srv_catalog_book_t *list = NULL;
    int count = 0;
    if (err == ESP_OK) {
        err = srv_catalog_fetch(&list, &count);
    }

    if (bsp_display_lock(1000) == ESP_OK) {
        if (s_root != NULL) {
            refresh_addr_locked();

            if (err == ESP_ERR_TIMEOUT) {
                lv_label_set_text(s_hint_lb, "没找到服务端（同网段？服务端在跑？）");
                lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_TEMP, 0);
            } else if (err != ESP_OK) {
                lv_label_set_text(s_hint_lb, "已发现服务端，但拉书单失败");
                lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_TEMP, 0);
            } else if (count == 0) {
                lv_label_set_text(s_hint_lb, "已发现服务端，书库是空的");
                lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_OK, 0);
            } else {
                srv_catalog_free(s_books);
                s_books = list;
                s_book_count = count;
                list = NULL;             /* 所有权转移，别在下面被 free */

                lv_obj_clean(s_list);
                for (int i = 0; i < count; i++) {
                    add_book_row(i);
                }

                char msg[64];
                snprintf(msg, sizeof(msg), "已发现服务端，%d 本书", count);
                lv_label_set_text(s_hint_lb, msg);
                lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_OK, 0);
            }
        }
        bsp_display_unlock();
    }

    srv_catalog_free(list);
    vTaskDelete(NULL);
}

/* ===================== 批量下载 ===================== */

static int s_batch_total;
static int s_batch_done;

static void batch_progress(int done, int total)
{
    if (bsp_display_lock(200) != ESP_OK) {
        return;
    }
    if (s_root != NULL && s_hint_lb != NULL) {
        char msg[64];
        snprintf(msg, sizeof(msg), "批量 %d/%d 本 · 本书 %d/%d 章",
                 s_batch_done + 1, s_batch_total, done, total);
        lv_label_set_text(s_hint_lb, msg);
        lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_PRIMARY, 0);
    }
    bsp_display_unlock();
}

static void batch_task(void *arg)
{
    (void)arg;

    int ok = 0;
    int fail = 0;

    for (int i = 0; i < s_book_count; i++) {
        if (srv_catalog_is_downloaded(&s_books[i])) {
            continue;               /* 已下载的跳过 */
        }
        esp_err_t err = srv_catalog_download(&s_books[i], batch_progress);
        if (err == ESP_OK) {
            ok++;
        } else {
            fail++;
        }
        s_batch_done++;
    }

    if (bsp_display_lock(1000) == ESP_OK) {
        if (s_root != NULL && s_hint_lb != NULL) {
            char msg[64];
            if (fail == 0) {
                snprintf(msg, sizeof(msg), "批量下载完成：%d 本", ok);
                lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_OK, 0);
            } else {
                snprintf(msg, sizeof(msg), "批量下载：成功 %d 本，失败 %d 本", ok, fail);
                lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_TEMP, 0);
            }
            lv_label_set_text(s_hint_lb, msg);
        }
        bsp_display_unlock();
    }

    s_downloading = false;
    vTaskDelete(NULL);
}

static void on_batch_confirm(void *arg)
{
    (void)arg;

    s_batch_total = 0;
    for (int i = 0; i < s_book_count; i++) {
        if (!srv_catalog_is_downloaded(&s_books[i])) {
            s_batch_total++;
        }
    }
    s_batch_done = 0;
    s_downloading = true;

    lv_label_set_text(s_hint_lb, "开始批量下载 ...");
    lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_PRIMARY, 0);

    if (xTaskCreate(batch_task, "ui_batch", 8192, NULL, 3, NULL) != pdPASS) {
        s_downloading = false;
        lv_label_set_text(s_hint_lb, "批量任务创建失败");
        lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_TEMP, 0);
    }
}

static void on_download_all(lv_event_t *e)
{
    (void)e;

    if (s_book_count == 0 || s_books == NULL) {
        lv_label_set_text(s_hint_lb, "书单是空的，先点「刷新」");
        lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_TEXT_DIM, 0);
        return;
    }
    if (s_downloading) {
        lv_label_set_text(s_hint_lb, "已有下载在进行");
        lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_TEMP, 0);
        return;
    }

    int pending = 0;
    for (int i = 0; i < s_book_count; i++) {
        if (!srv_catalog_is_downloaded(&s_books[i])) {
            pending++;
        }
    }
    if (pending == 0) {
        lv_label_set_text(s_hint_lb, "书单里的都已经下载了");
        lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_TEXT_DIM, 0);
        return;
    }

    char note[96];
    snprintf(note, sizeof(note), "还有 %d 本未下载，确认全部下载到 SD？", pending);
    ui_dialog_confirm("全部下载？", note, "下载", on_batch_confirm, NULL);
}

/* ===================== 下载 ===================== */

static void dl_progress(int done, int total)
{
    if (bsp_display_lock(200) != ESP_OK) {
        return;
    }
    if (s_root != NULL && s_hint_lb != NULL) {
        char msg[48];
        snprintf(msg, sizeof(msg), "下载中 %d / %d 章", done, total);
        lv_label_set_text(s_hint_lb, msg);
        lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_PRIMARY, 0);
    }
    bsp_display_unlock();
}

typedef struct {
    int idx;   /* s_books 下标 */
} dl_arg_t;

static void dl_task(void *arg)
{
    const int idx = ((dl_arg_t *)arg)->idx;
    free(arg);

    srv_catalog_book_t *b = &s_books[idx];
    esp_err_t err = srv_catalog_download(b, dl_progress);

    if (bsp_display_lock(1000) == ESP_OK) {
        if (s_root != NULL && s_hint_lb != NULL) {
            if (err == ESP_OK) {
                lv_label_set_text(s_hint_lb, "下载完成，可在书架里打开");
                lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_OK, 0);
            } else {
                lv_label_set_text(s_hint_lb,
                                  err == ESP_ERR_INVALID_STATE ? "SD 未挂载或服务端未配置"
                                                               : "下载失败（服务端没开？SD 满了？）");
                lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_TEMP, 0);
            }
        }
        bsp_display_unlock();
    }

    s_downloading = false;
    vTaskDelete(NULL);
}

/* 确认框点了「下载」之后真正起任务 */
static void on_dl_confirm(void *arg)
{
    const int idx = (int)(intptr_t)arg;
    if (idx < 0 || idx >= s_book_count || s_books == NULL || s_downloading) {
        return;
    }

    dl_arg_t *a = malloc(sizeof(*a));
    if (a == NULL) {
        return;
    }
    a->idx = idx;

    s_downloading = true;
    lv_label_set_text(s_hint_lb, "准备下载 ...");
    lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_PRIMARY, 0);

    if (xTaskCreate(dl_task, "ui_dl", 8192, a, 3, NULL) != pdPASS) {
        free(a);
        s_downloading = false;
        lv_label_set_text(s_hint_lb, "下载任务创建失败");
        lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_TEMP, 0);
    }
}

static void on_book_clicked(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_book_count || s_books == NULL) {
        return;
    }
    if (s_downloading) {
        lv_label_set_text(s_hint_lb, "已有下载在进行");
        lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_TEMP, 0);
        return;
    }

    const srv_catalog_book_t *b = &s_books[idx];
    char note[96];
    snprintf(note, sizeof(note), "《%.40s》%d 章，确认下载到 SD？", b->title, b->chapters);
    ui_dialog_confirm("下载这本书？", note, "下载", on_dl_confirm, (void *)(intptr_t)idx);
}

static void add_book_row(int idx)
{
    const srv_catalog_book_t *b = &s_books[idx];

    lv_obj_t *row = ui_row_create(s_list,
                                  (lv_coord_t)(lv_obj_get_child_count(s_list) * 44),
                                  &ic_book_open, b->title);
    if (row == NULL) {
        return;
    }
    ui_add_click_feedback(row);
    lv_obj_add_event_cb(row, on_book_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

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
            /* 换一页新书单：旧缓存先释放 */
            srv_catalog_free(s_books);
            s_books = NULL;
            s_book_count = 0;

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
                s_books = list;          /* 所有权转移给页面 */
                s_book_count = count;
                list = NULL;             /* 防止下面被 free */

                for (int i = 0; i < count; i++) {
                    add_book_row(i);
                }
                char msg[64];
                snprintf(msg, sizeof(msg), "共 %d 本，点书名下载", count);
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
    /* 书单缓存在页面销毁时释放；下载任务若还在跑，会看到 s_root == NULL
     * 而跳过 UI 更新，但 s_books 可能是它正在读的——所以等它结束再放。
     * 简化处理：下载中不释放（内存很小，下次进页面会重新拉）。 */
    if (!s_downloading) {
        srv_catalog_free(s_books);
        s_books = NULL;
        s_book_count = 0;
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

    /* 所有内容从标题栏下方开始（UI_TITLEBAR_H = 28）。
     * 之前直接写 y=4，第一行整个压在标题栏上。 */
    const lv_coord_t top = UI_TITLEBAR_H + 4;

    /* ---- 服务端地址 + 发现（同行） ---- */
    lv_obj_t *row = ui_row_create(root, top, &ic_wifi, "服务端");
    s_addr_lb = ui_row_value_create(row, -80, 150);
    lv_obj_set_style_text_color(s_addr_lb, UI_COLOR_TEXT_DIM, 0);
    ui_row_btn_create(row, -8, 56, "发现", on_discover, NULL);
    refresh_addr_locked();

    /* ---- 刷新书单 + 全部下载 ---- */
    row = ui_row_create(root, top + 44, NULL, "书单");
    ui_row_btn_create(row, -8, 52, "刷新", on_fetch, NULL);
    ui_row_btn_create(row, -64, 52, "全部", on_download_all, NULL);

    /* ---- 状态提示 ---- */
    s_hint_lb = lv_label_create(root);
    lv_obj_set_style_text_font(s_hint_lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_long_mode(s_hint_lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_hint_lb, ui_scr_w() - 20);
    lv_label_set_text(s_hint_lb, "点「发现」在同一 WiFi 下搜索");
    lv_obj_set_pos(s_hint_lb, 10, top + 88);

    /* ---- 书单（占据剩余高度） ---- */
    const lv_coord_t list_y = top + 118;
    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(list, ui_scr_w(), ui_scr_h() - list_y);
    lv_obj_set_pos(list, 0, list_y);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    s_list = list;

    lv_obj_add_event_cb(root, online_root_del_cb, LV_EVENT_DELETE, NULL);
}
