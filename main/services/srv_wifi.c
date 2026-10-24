#include "srv_wifi.h"

#include "srv_config.h"
#include "srv_sd.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "srv_wifi";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

#define CONNECT_RETRY_MAX   3
#define CONNECT_WAIT_MS     15000       /* 单次连接等待上限 */

/* 历史记录：一条 = SSID + 密码 */
typedef struct {
    char ssid[SRV_WIFI_SSID_MAX];
    char pass[SRV_WIFI_PASS_MAX];
} wifi_hist_t;

static EventGroupHandle_t s_evt      = NULL;
static esp_netif_t       *s_netif    = NULL;
static bool               s_inited   = false;
static bool               s_enabled  = false;
static bool               s_connected = false;
static bool               s_connecting = false;   /*!< 正在连接/重连 */
static int                s_retry    = 0;
static char               s_ip[16];
static char               s_ssid[SRV_WIFI_SSID_MAX];
static int8_t             s_rssi     = 0;

/*! 手动选网络期间置位：让后台自动连接让位，也让「主动断开」引起的事件不触发重连 */
static volatile bool      s_manual_busy = false;
/*! 自动连接任务收到它就该停手（用户手动连了别的网络 / 网络被关掉） */
static volatile bool      s_auto_stop   = true;

static wifi_hist_t s_hist[SRV_WIFI_HISTORY_MAX];
static int         s_hist_count = 0;
static char        s_hist_path[64];

/* ===================== 历史文件 ===================== */

static void hist_set_path(void)
{
    snprintf(s_hist_path, sizeof(s_hist_path), "%s/wifi.dat", srv_sd_mount_point());
}

static void hist_load(void)
{
    s_hist_count = 0;

    if (!srv_sd_is_mounted()) {
        ESP_LOGW(TAG, "SD 未挂载，网络历史不可用");
        return;
    }

    FILE *fp = fopen(s_hist_path, "r");
    if (fp == NULL) {
        ESP_LOGI(TAG, "网络历史不存在（%s），首次使用", s_hist_path);
        return;
    }

    char line[SRV_WIFI_SSID_MAX + SRV_WIFI_PASS_MAX + 8];
    while (fgets(line, sizeof(line), fp) != NULL && s_hist_count < SRV_WIFI_HISTORY_MAX) {
        /* 去掉行尾换行 */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0 || line[0] == '#') {
            continue;
        }

        char *tab = strchr(line, '\t');
        if (tab == NULL) {
            continue;               /* 缺分隔符，跳过 */
        }
        *tab = '\0';

        /* 用 "%.*s" 显式限长，避免 -Wstringop-truncation / -Wformat-truncation */
        snprintf(s_hist[s_hist_count].ssid, sizeof(s_hist[s_hist_count].ssid),
                 "%.*s", SRV_WIFI_SSID_MAX - 1, line);
        snprintf(s_hist[s_hist_count].pass, sizeof(s_hist[s_hist_count].pass),
                 "%.*s", SRV_WIFI_PASS_MAX - 1, tab + 1);
        s_hist_count++;
    }
    fclose(fp);

    ESP_LOGI(TAG, "网络历史 %d 条", s_hist_count);
}

static esp_err_t hist_save(void)
{
    if (!srv_sd_is_mounted()) {
        ESP_LOGW(TAG, "SD 未挂载，网络历史仅驻留内存");
        return ESP_ERR_INVALID_STATE;
    }

    FILE *fp = fopen(s_hist_path, "w");
    if (fp == NULL) {
        ESP_LOGE(TAG, "无法写入 %s", s_hist_path);
        return ESP_FAIL;
    }

    fprintf(fp, "# clock_reader wifi history, 格式: SSID\\tPASSWORD\n");
    for (int i = 0; i < s_hist_count; i++) {
        fprintf(fp, "%s\t%s\n", s_hist[i].ssid, s_hist[i].pass);
    }
    fclose(fp);
    return ESP_OK;
}

int srv_wifi_history_count(void)
{
    return s_hist_count;
}

esp_err_t srv_wifi_history_at(int idx, char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    if (idx < 0 || idx >= s_hist_count) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ssid && ssid_len) {
        snprintf(ssid, ssid_len, "%.*s", (int)(ssid_len - 1), s_hist[idx].ssid);
    }
    if (pass && pass_len) {
        snprintf(pass, pass_len, "%.*s", (int)(pass_len - 1), s_hist[idx].pass);
    }
    return ESP_OK;
}

esp_err_t srv_wifi_history_get(const char *ssid, char *pass, size_t pass_len)
{
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < s_hist_count; i++) {
        if (strcmp(s_hist[i].ssid, ssid) == 0) {
            if (pass && pass_len) {
                snprintf(pass, pass_len, "%s", s_hist[i].pass);
            }
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t srv_wifi_history_add(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    pass = (pass == NULL) ? "" : pass;

    /* 已存在则先移除，再插到最前，保持「最近使用优先」 */
    for (int i = 0; i < s_hist_count; i++) {
        if (strcmp(s_hist[i].ssid, ssid) == 0) {
            for (int j = i; j < s_hist_count - 1; j++) {
                s_hist[j] = s_hist[j + 1];
            }
            s_hist_count--;
            break;
        }
    }

    int n = (s_hist_count < SRV_WIFI_HISTORY_MAX) ? s_hist_count + 1 : SRV_WIFI_HISTORY_MAX;
    for (int i = n - 1; i > 0; i--) {
        s_hist[i] = s_hist[i - 1];
    }
    strncpy(s_hist[0].ssid, ssid, SRV_WIFI_SSID_MAX - 1);
    s_hist[0].ssid[SRV_WIFI_SSID_MAX - 1] = '\0';
    strncpy(s_hist[0].pass, pass, SRV_WIFI_PASS_MAX - 1);
    s_hist[0].pass[SRV_WIFI_PASS_MAX - 1] = '\0';
    s_hist_count = n;

    return hist_save();
}

esp_err_t srv_wifi_history_remove(const char *ssid)
{
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < s_hist_count; i++) {
        if (strcmp(s_hist[i].ssid, ssid) == 0) {
            for (int j = i; j < s_hist_count - 1; j++) {
                s_hist[j] = s_hist[j + 1];
            }
            s_hist_count--;
            return hist_save();
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t srv_wifi_history_clear(void)
{
    s_hist_count = 0;
    return hist_save();
}

/* ===================== 事件处理 ===================== */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id == WIFI_EVENT_STA_START) {
        ESP_LOGD(TAG, "STA 启动");
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "已关联 AP，等待分配 IP");
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
        s_connected = false;
        s_ip[0] = '\0';
        ESP_LOGW(TAG, "断开连接，reason=%d", d ? d->reason : -1);

        if (s_manual_busy) {
            /* 这次断开是我们为了换网络主动断的，别去重连旧 AP */
            ESP_LOGI(TAG, "切换网络中，跳过自动重连");
            return;
        }
        if (!s_enabled) {
            s_connecting = false;
            return;
        }
        if (s_retry < CONNECT_RETRY_MAX) {
            s_retry++;
            s_connecting = true;
            ESP_LOGI(TAG, "重连第 %d/%d 次", s_retry, CONNECT_RETRY_MAX);
            esp_wifi_connect();
        } else {
            s_connecting = false;
            xEventGroupSetBits(s_evt, WIFI_FAIL_BIT);
        }
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }

    const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
    s_connected  = true;
    s_connecting = false;
    s_retry      = 0;

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_rssi = ap.rssi;
        snprintf(s_ssid, sizeof(s_ssid), "%s", (const char *)ap.ssid);
    }

    ESP_LOGI(TAG, "获取 IP: %s  RSSI %d dBm", s_ip, s_rssi);
    xEventGroupSetBits(s_evt, WIFI_CONNECTED_BIT);
}

/* ===================== 生命周期 ===================== */

esp_err_t srv_wifi_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    hist_set_path();
    hist_load();

    s_evt = xEventGroupCreate();
    if (s_evt == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) {
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_ip_event, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    s_inited = true;
    ESP_LOGI(TAG, "WiFi 就绪（历史 %d 条，开关 %s）",
             s_hist_count, srv_config_get_int(CFG_WIFI_EN, 0) ? "开" : "关");
    return ESP_OK;
}

bool srv_wifi_is_enabled(void)
{
    return s_enabled;
}

esp_err_t srv_wifi_set_enabled(bool on)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!on) {
        s_enabled    = false;
        s_connecting = false;
        s_auto_stop  = true;        /* 关网等于叫停自动连接 */
        if (s_connected) {
            esp_wifi_disconnect();
        }
        esp_wifi_stop();
        s_connected = false;
        s_ip[0] = '\0';
        ESP_LOGI(TAG, "网络已关闭");
        return srv_config_set_int(CFG_WIFI_EN, 0, true);
    }

    s_enabled = true;
    s_retry   = 0;
    xEventGroupClearBits(s_evt, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    esp_err_t ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(TAG, "esp_wifi_start 失败: %s", esp_err_to_name(ret));
        s_enabled = false;
        return ret;
    }

    ESP_LOGI(TAG, "网络已开启");
    srv_config_set_int(CFG_WIFI_EN, 1, false);
    srv_config_save_deferred();

    /* 有历史记录就走自动连接流程 */
    if (s_hist_count > 0) {
        srv_wifi_autoconnect();
    }
    return ESP_OK;
}

/* ===================== 状态 ===================== */

bool srv_wifi_is_connected(void)
{
    return s_connected;
}

bool srv_wifi_is_connecting(void)
{
    return s_connecting;
}

esp_err_t srv_wifi_get_ip(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_connected) {
        out[0] = '\0';
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(out, out_len, "%s", s_ip);
    return ESP_OK;
}

int8_t srv_wifi_get_rssi(void)
{
    return s_connected ? s_rssi : 0;
}

const char *srv_wifi_get_ssid(void)
{
    return s_connected ? s_ssid : "";
}

/* ===================== 扫描与连接 ===================== */

esp_err_t srv_wifi_scan(void (*cb)(const srv_wifi_ap_t *ap, void *arg), void *arg, int *ap_count)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ap_count) {
        *ap_count = 0;
    }

    const wifi_scan_config_t scan_cfg = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };

    esp_err_t ret = esp_wifi_scan_start(&scan_cfg, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "扫描失败: %s", esp_err_to_name(ret));
        return ret;
    }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);

    /* 即使 0 条也要调用 get_ap_records 释放驱动内部缓存；
     * 分配至少 1 个元素，避免向 API 传 NULL。 */
    wifi_ap_record_t *recs = calloc(num ? num : 1, sizeof(wifi_ap_record_t));
    if (recs == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint16_t got = num;
    ret = esp_wifi_scan_get_ap_records(&got, recs);
    if (ret == ESP_OK && cb) {
        for (uint16_t i = 0; i < got; i++) {
            srv_wifi_ap_t ap = {0};
            snprintf(ap.ssid, sizeof(ap.ssid), "%s", (const char *)recs[i].ssid);
            ap.rssi     = recs[i].rssi;
            ap.authmode = (uint8_t)recs[i].authmode;
            cb(&ap, arg);
        }
    }
    if (ap_count) {
        *ap_count = (ret == ESP_OK) ? (int)got : 0;
    }

    free(recs);
    return ret;
}

esp_err_t srv_wifi_connect(const char *ssid, const char *pass, bool save, bool manual)
{
    if (!s_inited || ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    pass = (pass == NULL) ? "" : pass;

    if (manual) {
        /* 用户手动选的网络说了算：叫停后台自动连接。
         * 不叫停的话，它会等我们连上之后再断开、去试历史里的下一条，
         * 表现就是"点了连接没反应，得把网络关了再开才能连上"。 */
        s_auto_stop = true;
    }

    if (!s_enabled) {
        esp_err_t ret = srv_wifi_set_enabled(true);
        if (ret != ESP_OK) {
            return ret;
        }
        if (manual) {
            s_auto_stop = true;     /* set_enabled 里又起了一轮自动连接，重新叫停 */
        }
    }

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;     /* 接受任意加密方式 */
    wc.sta.pmf_cfg.capable    = true;
    wc.sta.pmf_cfg.required   = false;

    /* 换网络必须先断开：STA 已经连着别的 AP 时再 esp_wifi_connect 是无效的
     * （IDF 文档明确要求先 disconnect），这就是"要重新开关网络才能连到新的"。
     * s_manual_busy 挡住这次主动断开触发的事件重连，否则它会拿旧配置连回去。 */
    s_manual_busy = true;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(30));          /* 让 wifi 任务把断开处理完 */

    s_retry = 0;
    s_connecting = true;
    xEventGroupClearBits(s_evt, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    esp_err_t ret = esp_wifi_connect();
    s_manual_busy = false;

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "连接 %s 失败: %s", ssid, esp_err_to_name(ret));
        s_connecting = false;
        return ret;
    }
    ESP_LOGI(TAG, "正在连接 %s ...", ssid);

    if (save) {
        srv_config_set_str(CFG_WIFI_SSID, ssid, false);
        srv_config_set_str(CFG_WIFI_PASS, pass, false);
        srv_config_save_deferred();
        srv_wifi_history_add(ssid, pass);
    }
    return ESP_OK;
}

esp_err_t srv_wifi_disconnect(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    s_enabled    = false;              /* 抑制自动重连 */
    s_connecting = false;
    s_auto_stop  = true;
    return esp_wifi_disconnect();
}

/* ===================== 开机自动连接 ===================== */

static void autoconnect_task(void *arg)
{
    (void)arg;
    char pass[SRV_WIFI_PASS_MAX];
    int  found = 0;

    /* 任务可能刚起来就被手动连接叫停了，别白扫一遍 */
    if (s_auto_stop || !s_enabled) {
        ESP_LOGI(TAG, "自动连接被取消");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "自动连接：开始扫描");
    if (srv_wifi_scan(NULL, NULL, &found) != ESP_OK) {
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "扫描到 %d 个网络，按历史顺序匹配", found);

    for (int i = 0; i < s_hist_count; i++) {
        /* 用户自己连了别的网络，或者网络被关掉：立刻停手 */
        if (s_auto_stop || !s_enabled) {
            ESP_LOGI(TAG, "自动连接被取消");
            vTaskDelete(NULL);
            return;
        }

        char ssid[SRV_WIFI_SSID_MAX];
        if (srv_wifi_history_at(i, ssid, sizeof(ssid), pass, sizeof(pass)) != ESP_OK) {
            continue;
        }

        ESP_LOGI(TAG, "尝试历史第 %d 条: %s", i + 1, ssid);
        if (srv_wifi_connect(ssid, pass, false, false) != ESP_OK) {
            continue;
        }

        EventBits_t bits = xEventGroupWaitBits(s_evt, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                               pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(CONNECT_WAIT_MS));
        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "自动连接成功: %s", ssid);
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGW(TAG, "连接 %s 超时/失败，试下一条", ssid);
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    ESP_LOGW(TAG, "自动连接：历史中无可连接网络");
    vTaskDelete(NULL);
}

esp_err_t srv_wifi_autoconnect(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    s_auto_stop = false;               /* 新的一轮自动连接，重新开始 */
    if (xTaskCreate(autoconnect_task, "wifi_auto", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
