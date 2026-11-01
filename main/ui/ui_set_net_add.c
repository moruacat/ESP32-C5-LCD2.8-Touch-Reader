#include "ui_set_pages.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "ui_dialog.h"
#include "ui_ime.h"                   /* 手动输入 SSID 用中文输入层 */
#include "app_navigator.h"
#include "srv_wifi.h"

#include "bsp/esp32_c5_touch_lcd_2.8.h"
#include "esp_log.h"
#include "esp_wifi.h"                 /* WIFI_AUTH_OPEN */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_net_add";

/*! 扫描结果上限：一屏放不下就滚动，24 条够用 */
#define AP_MAX       24
/*! 列表前两行是固定的（第 0 行「扫描网络」、第 1 行「手动添加网络」），
 *  热点从第 2 行往下排 */
#define AP_ROW_BASE  2

/*! 扫描重试：射频忙（连接中、上一次扫描没收尾）时会直接失败，多试几次
 *  比让用户跑出去把网络关了再开强；空结果也重试一次，免得一帧没收到就报"未发现" */
#define SCAN_TRY_MAX   5
#define SCAN_RETRY_MS  700
/*! 进页面时若正在连接，最多等这么久再开始扫（10*300ms = 3 秒） */
#define SCAN_WAIT_CONN 10

static srv_wifi_ap_t s_aps[AP_MAX];
static int           s_ap_count;
static bool          s_scanning;            /*!< 有扫描任务在跑（同时只允许一个） */
static bool          s_scan_fail;           /*!< 上一次扫描是失败的（区别于"没热点"） */

/* 扫描放在独立任务里（同步扫描要 2~6 秒，搁 LVGL 任务里界面会僵住），
 * 结果回来时页面可能已经被切走 —— 靠 s_list 判断还该不该动界面。 */
static lv_obj_t *s_list;
static lv_obj_t *s_scan_state;

/* ===================== 连接 ===================== */

static void connect_to(const char *ssid, const char *pass)
{
    pass = (pass == NULL) ? "" : pass;

    /* save = true：写进 config.dat，同时记进 /wifi.dat（下次开机自动连）
     * manual = true：先断开当前连接再连，并让后台自动连接让位 */
    if (srv_wifi_connect(ssid, pass, true, true) != ESP_OK) {
        ui_alert_show("连接失败", UI_COLOR_TEMP, ssid, "知道了", NULL, NULL, 0);
        return;
    }

    ESP_LOGI(TAG, "连接 %s", ssid);
    app_navigator_back();       /* 回网络页：那边每秒刷一次「当前网络」 */
}

/* 密码输完了（空密码也放行，用户自己知道在干什么） */
static void on_pass_input(const char *text, void *arg)
{
    connect_to((const char *)arg, text);
}

/* ===================== 手动添加（隐藏热点 / 中文名） ===================== */

/* SSID 先抄进这里再问密码。列表里的 ap->ssid 也一直有效，但那是"扫出来的"，
 * 手动这条没有对应的热点条目，用一个静态缓冲区最省事 */
static char s_manual_ssid[SRV_WIFI_SSID_MAX];

/* 名字输完了（空名字没法连，直接忽略） */
static void on_manual_ssid(const char *text, void *arg)
{
    (void)arg;

    if (text == NULL || text[0] == '\0') {
        return;
    }
    snprintf(s_manual_ssid, sizeof(s_manual_ssid), "%s", text);

    /* SSID 可能带中文（汉字 Wi-Fi 很常见），所以密码层也走中文输入层 */
    char title[SRV_WIFI_SSID_MAX + 16];
    snprintf(title, sizeof(title), "密码：%s", s_manual_ssid);
    ui_ime_text_input_show(title, SRV_WIFI_PASS_MAX - 1, on_pass_input, s_manual_ssid);
}

/* 点「手动添加网络」那行：先问名字，名字有了再问密码 */
static void on_manual_add(lv_event_t *e)
{
    (void)e;
    ui_ime_text_input_show("网络名称", SRV_WIFI_SSID_MAX - 1, on_manual_ssid, NULL);
}

static void on_ap_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_ap_count) {
        return;
    }

    /* 扫描还在跑也照连：一边扫一边连只会让扫描被中止（IDF 的行为），
     * 连接本身不受影响，这里没必要拦用户 */
    const srv_wifi_ap_t *ap = &s_aps[idx];
    if (ap->authmode == WIFI_AUTH_OPEN) {
        connect_to(ap->ssid, "");
        return;
    }

    char title[SRV_WIFI_SSID_MAX + 16];
    snprintf(title, sizeof(title), "密码：%s", ap->ssid);
    /* arg 指向 s_aps 里的 SSID：输密码时列表被弹层压着，扫不了也不会重排 */
    ui_ime_text_input_show(title, SRV_WIFI_PASS_MAX - 1, on_pass_input, (void *)ap->ssid);
}

/* ===================== 扫描 ===================== */

static void scan_state_refresh(void)
{
    if (s_scan_state == NULL) {
        return;
    }

    char buf[24];
    if (s_scanning) {
        snprintf(buf, sizeof(buf), "扫描中…");
    } else if (s_scan_fail) {
        snprintf(buf, sizeof(buf), "扫描失败");
    } else if (s_ap_count > 0) {
        snprintf(buf, sizeof(buf), "共 %d 个", s_ap_count);
    } else {
        snprintf(buf, sizeof(buf), "未发现");
    }
    lv_label_set_text(s_scan_state, buf);
}

/* 同一个 SSID 可能有好几个热点（mesh / 中继），只留信号最强的那个 */
static void ap_collect(const srv_wifi_ap_t *ap, void *arg)
{
    (void)arg;

    if (ap->ssid[0] == '\0') {
        return;                 /* 隐藏热点没有名字，列出来也没法选 */
    }

    for (int i = 0; i < s_ap_count; i++) {
        if (strcmp(s_aps[i].ssid, ap->ssid) == 0) {
            if (ap->rssi > s_aps[i].rssi) {
                s_aps[i] = *ap;
            }
            return;
        }
    }

    if (s_ap_count < AP_MAX) {
        s_aps[s_ap_count++] = *ap;
    }
}

static void ap_row_add(int idx)
{
    const srv_wifi_ap_t *ap = &s_aps[idx];
    bool is_cur = srv_wifi_is_connected() &&
                  (strcmp(srv_wifi_get_ssid(), ap->ssid) == 0);

    lv_obj_t *row = ui_row_create(s_list, UI_ROW_Y(AP_ROW_BASE + idx), &ic_wifi, ap->ssid);
    if (is_cur) {
        /* 已经连着的就不给按钮了，点了也只是重连同一个 */
        ui_row_btn_create(row, -8, 64, "已连接", NULL, NULL);
    } else {
        ui_row_btn_create(row, -8, 64, "连接", on_ap_click, (void *)(intptr_t)idx);
    }
}

/* 重建热点行（「扫描网络」那行留着）。需持有 LVGL 锁 */
static void list_fill(void)
{
    if (s_list == NULL) {
        return;
    }

    /* 从 AP_ROW_BASE 往后删：前面那几行是固定行（扫描 / 手动添加），
     * 重建往往就是点其中一行触发的，没必要把正在派发事件的控件删掉 */
    while (lv_obj_get_child_count(s_list) > AP_ROW_BASE) {
        lv_obj_delete(lv_obj_get_child(s_list, AP_ROW_BASE));
    }

    for (int i = 0; i < s_ap_count; i++) {
        ap_row_add(i);
    }
    scan_state_refresh();
}

static void scan_task(void *arg)
{
    (void)arg;

    /* 网络关着就顺手打开：不然扫描必然失败，用户还得退出去开开关 */
    if (!srv_wifi_is_enabled()) {
        srv_wifi_set_enabled(true);
        vTaskDelay(pdMS_TO_TICKS(600));
    }

    /* 正在连（或正在重连）别的网络时射频不接受扫描，先等这一轮连接结束 ——
     * 不然刚添加完返回再进来，扫描必然失败，看着就像"要重开网络才能扫" */
    for (int i = 0; i < SCAN_WAIT_CONN && srv_wifi_is_connecting(); i++) {
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    esp_err_t ret = ESP_FAIL;
    int empty_left = 1;                 /* 空结果只重试一次 */
    for (int i = 0; i < SCAN_TRY_MAX; i++) {
        /* 射频正在连（重连）时，扫描要么直接报忙、要么扫回 0 条，
         * 先等它忙完再扫 */
        for (int w = 0; w < 5 && srv_wifi_is_connecting(); w++) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        s_ap_count = 0;
        ret = srv_wifi_scan(ap_collect, NULL, NULL);
        if (ret == ESP_OK && s_ap_count > 0) {
            break;
        }
        if (ret == ESP_OK && empty_left-- <= 0) {
            break;                      /* 确实没热点，别一直转 */
        }
        vTaskDelay(pdMS_TO_TICKS(SCAN_RETRY_MS));
    }

    s_scan_fail = (ret != ESP_OK);
    if (s_scan_fail) {
        ESP_LOGW(TAG, "扫描失败: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "扫到 %d 个热点", s_ap_count);
    }

    /* 回 LVGL 上下文改界面：页面已经被切走的话 s_list 是空的，什么都不做 */
    if (bsp_display_lock(UI_LOCK_TIMEOUT_MS) == ESP_OK) {
        s_scanning = false;
        list_fill();
        bsp_display_unlock();
    } else {
        s_scanning = false;
        ESP_LOGW(TAG, "取 LVGL 锁失败，扫描结果未上屏");
    }

    vTaskDelete(NULL);
}

static void scan_start(void)
{
    if (s_scanning) {
        return;                 /* 上一次还没回来，等它填结果 */
    }

    s_scanning = true;
    if (xTaskCreate(scan_task, "net_scan", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "扫描任务创建失败");
        s_scanning = false;
    }
    scan_state_refresh();
}

static void on_rescan(lv_event_t *e)
{
    (void)e;
    scan_start();
}

/* ===================== 页面 ===================== */

static void list_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    /* 手快「返回又进来」时新页面已经建好了，旧页面的回调不能把新指针清掉 */
    if (lv_event_get_target(e) != s_list) {
        return;
    }
    s_list       = NULL;
    s_scan_state = NULL;
}

void ui_set_net_add_create(lv_obj_t *root)
{
    s_scan_state = NULL;
    /* 上次的结果作废：进页面就是"列表空着 + 状态显示扫描中"，扫完一次性填上，
     * 中间不会先闪一遍旧列表。扫描任务还在跑（结果马上就回来）时不动它。 */
    if (!s_scanning) {
        s_ap_count  = 0;
        s_scan_fail = false;
    }

    ui_titlebar_create(root, "添加网络", app_navigator_back_cb, NULL);

    s_list = ui_settings_list_create(root);
    lv_obj_add_event_cb(s_list, list_del_cb, LV_EVENT_DELETE, NULL);

    /* 第 0 行：整行可点，点一下重新扫，右侧显示进度/结果 */
    lv_obj_t *row = ui_row_create(s_list, UI_ROW_Y(0), &ic_search, "扫描网络");
    ui_add_click_feedback(row);
    lv_obj_add_event_cb(row, on_rescan, LV_EVENT_CLICKED, NULL);
    s_scan_state = ui_row_value_create(row, -12, 96);
    lv_obj_set_style_text_color(s_scan_state, UI_COLOR_TEXT_DIM, 0);

    /* 第 1 行：手动输入网络名。隐藏热点（不广播 SSID）扫不出来，中文名的热点
     * 也未必能在下面那串里认出来，都从这儿进。
     * 必须排在扫描行后面建：删除/重建热点行是按孩子下标来的 */
    lv_obj_t *man = ui_row_create(s_list, UI_ROW_Y(1), &ic_wifi, "手动添加网络");
    ui_add_click_feedback(man);
    lv_obj_add_event_cb(man, on_manual_add, LV_EVENT_CLICKED, NULL);
    lv_obj_t *man_hint = ui_row_value_create(man, -12, 96);
    lv_label_set_text(man_hint, "输入名称");
    lv_obj_set_style_text_color(man_hint, UI_COLOR_TEXT_DIM, 0);

    scan_start();               /* 里面会把状态文字刷成「扫描中…」 */
}
