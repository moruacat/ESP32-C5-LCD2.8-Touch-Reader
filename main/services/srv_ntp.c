#include "srv_ntp.h"

#include "srv_config.h"
#include "srv_rtc.h"
#include "srv_wifi.h"

#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "srv_ntp";

/* 依次尝试的 NTP 服务器 */
static const char *NTP_SERVERS[] = {
    "ntp.aliyun.com",
    "cn.pool.ntp.org",
    "time.windows.com",
};
#define NTP_SERVER_NUM  (sizeof(NTP_SERVERS) / sizeof(NTP_SERVERS[0]))

#define WAIT_ONLINE_MAX_MS   300000     /* 等联网上限 5 分钟 */
#define WAIT_SYNC_MAX_MS     30000      /* 等单次同步上限 30 秒 */
#define SYNC_INTERVAL_S      21600      /* 每 6 小时重新联网校时一次 */

static bool   s_inited      = false;
static bool   s_started     = false;
static bool   s_synced      = false;
static char   s_last_sync[24];

/* 把系统时间（UTC 秒 + 时区 + 微调）换算成本地时间写入 RTC。
 * 这里不用 POSIX TZ 字符串（其符号与直觉相反，容易搞反）。 */
esp_err_t srv_ntp_write_rtc(void)
{
    time_t utc = time(NULL);
    if (utc < 1700000000) {             /* 2000 年之前视为未同步 */
        return ESP_ERR_INVALID_STATE;
    }

    /* 本地时间 = UTC + 时区（小时）+ 秒级微调 */
    time_t local = utc
                 + (time_t)srv_config_get_int(CFG_NTP_TZ,   CFG_DEF_NTP_TZ) * 3600
                 + (time_t)srv_config_get_int(CFG_NTP_FINE, CFG_DEF_NTP_FINE);
    struct tm tmv;
    if (gmtime_r(&local, &tmv) == NULL) {
        return ESP_FAIL;
    }

    srv_rtc_time_t t = {
        .year  = tmv.tm_year + 1900,
        .month = tmv.tm_mon + 1,
        .day   = tmv.tm_mday,
        .hour  = tmv.tm_hour,
        .min   = tmv.tm_min,
        .sec   = tmv.tm_sec,
    };
    t.dotw = srv_rtc_calc_dotw(t.year, t.month, t.day);

    esp_err_t ret = srv_rtc_set_time(&t);
    if (ret == ESP_OK) {
        snprintf(s_last_sync, sizeof(s_last_sync), "%04d-%02d-%02d %02d:%02d:%02d",
                 t.year, t.month, t.day, t.hour, t.min, t.sec);
        ESP_LOGI(TAG, "RTC 已校时为 %s", s_last_sync);
    } else {
        ESP_LOGE(TAG, "写 RTC 失败: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t srv_ntp_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    for (size_t i = 0; i < NTP_SERVER_NUM; i++) {
        esp_sntp_setservername((uint8_t)i, NTP_SERVERS[i]);
    }

    s_inited = true;
    ESP_LOGI(TAG, "NTP 就绪（服务器 %s 等 %d 个）", NTP_SERVERS[0], (int)NTP_SERVER_NUM);
    return ESP_OK;
}

/* 超时时打印诊断，区分「域名没解析出来」和「解析出来了但服务器不应答」 */
static void ntp_log_timeout(uint32_t timeout_ms)
{
    const char *name = esp_sntp_getservername(0);
    const ip_addr_t *ip = esp_sntp_getserver(0);

    if (name == NULL) {
        name = "(未设置)";
    }

    if (ip != NULL && !ip_addr_isany(ip)) {
        ESP_LOGW(TAG, "校时超时（%u ms）：%s 已解析为 %s 但无应答，多为网络屏蔽 UDP 123",
                 (unsigned)timeout_ms, name, ipaddr_ntoa(ip));
    } else {
        ESP_LOGW(TAG, "校时超时（%u ms）：%s 域名未解析成功，检查外网与 DNS",
                 (unsigned)timeout_ms, name);
    }
}

esp_err_t srv_ntp_sync_now(uint32_t timeout_ms)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!srv_wifi_is_connected()) {
        ESP_LOGW(TAG, "未联网，无法校时");
        return ESP_ERR_INVALID_STATE;
    }

    /* lwip 的 sntp_init() 在客户端已启动时是空操作，所以每次手动校时都要
     * 先 stop 再 init，才会重新发一次请求；否则要等 sync_interval
     * （默认 1 小时）才轮到下一次轮询，表现就是「手动校时必然超时」。 */
    if (s_started) {
        esp_sntp_stop();
    }
    esp_sntp_init();
    s_started = true;

    /* sntp_get_sync_status() 是「读取即清零」：
     * 读到 COMPLETED 的同时状态就被重置为 RESET。
     * 因此绝不能读第二次来判断结果，否则永远判成超时。 */
    (void)esp_sntp_get_sync_status();

    const TickType_t step = pdMS_TO_TICKS(200);
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            s_synced = true;
            return srv_ntp_write_rtc();
        }
        vTaskDelay(step);
        waited += 200;
    }

    ntp_log_timeout(timeout_ms);
    return ESP_ERR_TIMEOUT;
}

esp_err_t srv_ntp_stop(void)
{
    if (!s_started) {
        return ESP_OK;
    }
    esp_sntp_stop();
    s_started = false;
    return ESP_OK;
}

bool srv_ntp_is_synced(void)
{
    return s_synced;
}

const char *srv_ntp_last_sync_text(void)
{
    return s_last_sync;
}

/* ===================== 自动流程 ===================== */

static void ntp_task(void *arg)
{
    (void)arg;

    /* 1. 等待联网 */
    uint32_t waited = 0;
    while (!srv_wifi_is_connected() && waited < WAIT_ONLINE_MAX_MS) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        waited += 1000;
    }
    if (!srv_wifi_is_connected()) {
        ESP_LOGW(TAG, "等待联网超时，本次不做 NTP 校时");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "已联网，开始 NTP 校时");

    /* 2. 首次校时 */
    if (srv_ntp_sync_now(WAIT_SYNC_MAX_MS) != ESP_OK) {
        ESP_LOGW(TAG, "首次校时失败");
        vTaskDelete(NULL);
        return;
    }

    /* 3. 之后每 6 小时重新联网校时一次；连不上就退回「只回写 RTC」，
     *    至少不让 RTC 的走时偏差继续累积 */
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(SYNC_INTERVAL_S * 1000));
        if (!srv_wifi_is_connected()) {
            continue;
        }
        if (srv_ntp_sync_now(WAIT_SYNC_MAX_MS) != ESP_OK) {
            ESP_LOGW(TAG, "定期校时失败，改为回写 RTC");
            srv_ntp_write_rtc();
        }
    }
}

esp_err_t srv_ntp_autostart(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (srv_config_get_int(CFG_NTP_EN, 1) == 0) {
        ESP_LOGI(TAG, "NTP 开关为关，跳过");
        return ESP_OK;
    }
    if (xTaskCreate(ntp_task, "srv_ntp", 4096, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
