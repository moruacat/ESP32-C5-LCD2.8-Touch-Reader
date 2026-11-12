#include "srv_ai_tools.h"

#include "srv_audio.h"
#include "srv_battery.h"
#include "srv_config.h"
#include "srv_player.h"
#include "srv_rtc.h"
#include "srv_ntp.h"
#include "srv_safe.h"
#include "srv_sd.h"
#include "srv_wifi.h"

#include "ui_common.h"
#include "ui_dialog.h"

#include "bsp/esp32_c5_touch_lcd_2.8.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "srv_ai_tool";

/* ===================== 状态查询 ===================== */

static esp_mcp_value_t tool_status(const esp_mcp_property_list_t *properties)
{
    (void)properties;

    char buf[320];
    int n = 0;

    float v = 0.0f;
    int   pct = 0;
    if (srv_battery_get(&v, &pct) == ESP_OK) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                      "电量 %d%%（%.2fV）", pct, (double)v);
    } else {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "电量未知");
    }

    if (srv_wifi_is_enabled() && srv_wifi_is_connected()) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                      "，WiFi 已连接（%s）", srv_wifi_get_ssid());
    } else if (srv_wifi_is_enabled()) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "，WiFi 未连接");
    } else {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "，WiFi 已关闭");
    }

    srv_rtc_time_t t;
    if (srv_rtc_get_time(&t) == ESP_OK) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                      "，时间 %04d-%02d-%02d %02d:%02d",
                      t.year, t.month, t.day, t.hour, t.min);
    }

    n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                  "，SD 卡%s", srv_sd_is_mounted() ? "已挂载" : "未挂载");
    n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                  "，音量 %d%%，亮度 %d%%",
                  srv_audio_get_volume(),
                  (int)srv_config_get_int(CFG_BRIGHTNESS, 70));

    switch (srv_player_state()) {
    case SRV_PLAYER_PLAYING: n += snprintf(buf + n, sizeof(buf) - (size_t)n, "，音乐播放中"); break;
    case SRV_PLAYER_PAUSED:  n += snprintf(buf + n, sizeof(buf) - (size_t)n, "，音乐已暂停"); break;
    default:                 n += snprintf(buf + n, sizeof(buf) - (size_t)n, "，音乐未播放"); break;
    }

    if (srv_safe_on()) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "，安全模式开启");
    }

    return esp_mcp_value_create_string(buf);
}

/* ===================== 音量 / 亮度 ===================== */

static esp_mcp_value_t tool_set_volume(const esp_mcp_property_list_t *properties)
{
    int vol = esp_mcp_property_list_get_property_int(properties, "volume");
    if (vol < 0 || vol > 100) {
        return esp_mcp_value_create_bool(false);
    }
    (void)srv_audio_set_volume(vol);
    ESP_LOGI(TAG, "AI 设置音量 %d%%", vol);
    return esp_mcp_value_create_bool(true);
}

static esp_mcp_value_t tool_set_brightness(const esp_mcp_property_list_t *properties)
{
    int b = esp_mcp_property_list_get_property_int(properties, "brightness");
    if (b < 1 || b > 100) {
        return esp_mcp_value_create_bool(false);
    }
    srv_config_set_int(CFG_BRIGHTNESS, b, false);
    srv_config_save_deferred();
    bsp_display_brightness_set(b);
    ESP_LOGI(TAG, "AI 设置亮度 %d%%", b);
    return esp_mcp_value_create_bool(true);
}

/* ===================== 时间 ===================== */

static esp_mcp_value_t tool_set_time(const esp_mcp_property_list_t *properties)
{
    int hour = esp_mcp_property_list_get_property_int(properties, "hour");
    int min  = esp_mcp_property_list_get_property_int(properties, "minute");

    if (hour < 0 || hour > 23 || min < 0 || min > 59) {
        return esp_mcp_value_create_bool(false);
    }

    srv_rtc_time_t t;
    if (srv_rtc_get_time(&t) != ESP_OK) {
        return esp_mcp_value_create_bool(false);
    }
    t.hour = hour;
    t.min  = min;
    t.sec  = 0;
    if (srv_rtc_set_time(&t) != ESP_OK) {
        return esp_mcp_value_create_bool(false);
    }
    /* AI 手动设置的是新的当前时间，重置每日偏移基准，避免跨天重复补正。 */
    srv_ntp_daily_reset_anchor();
    ESP_LOGI(TAG, "AI 设置时间 %02d:%02d", hour, min);
    return esp_mcp_value_create_bool(true);
}

/* ===================== 屏幕提示 ===================== */

static esp_mcp_value_t tool_show_message(const esp_mcp_property_list_t *properties)
{
    const char *text = esp_mcp_property_list_get_property_string(properties, "text");
    if (text == NULL || text[0] == '\0') {
        return esp_mcp_value_create_bool(false);
    }

    /* LLM 可能给很长一段，提示层只有一行说明，截到 180 字节 */
    char msg[192];
    snprintf(msg, sizeof(msg), "%.180s", text);

    /* 回调在组件任务里，弹层必须在 LVGL 锁内建 */
    if (bsp_display_lock(1000) != ESP_OK) {
        return esp_mcp_value_create_bool(false);
    }
    ui_alert_show("小智", UI_COLOR_PRIMARY, msg, "知道了", NULL, NULL, 4000);
    bsp_display_unlock();

    return esp_mcp_value_create_bool(true);
}

/* ===================== 注册 ===================== */

void srv_ai_tools_register(esp_mcp_t *mcp)
{
    if (mcp == NULL) {
        return;
    }

    esp_mcp_tool_t *tool = NULL;
    esp_mcp_property_t *prop = NULL;

    tool = esp_mcp_tool_create("self.get_device_status",
                               "Get device status: battery, Wi-Fi, SD card, time, volume, brightness, music state",
                               tool_status);
    if (tool != NULL) {
        (void)esp_mcp_add_tool(mcp, tool);
    }

    tool = esp_mcp_tool_create("self.audio_speaker.set_volume",
                               "Set speaker volume, 0-100", tool_set_volume);
    if (tool != NULL) {
        prop = esp_mcp_property_create_with_range("volume", 0, 100);
        if (prop != NULL) {
            (void)esp_mcp_tool_add_property(tool, prop);
        }
        (void)esp_mcp_add_tool(mcp, tool);
    }

    tool = esp_mcp_tool_create("self.screen.set_brightness",
                               "Set screen brightness, 1-100", tool_set_brightness);
    if (tool != NULL) {
        prop = esp_mcp_property_create_with_range("brightness", 1, 100);
        if (prop != NULL) {
            (void)esp_mcp_tool_add_property(tool, prop);
        }
        (void)esp_mcp_add_tool(mcp, tool);
    }

    tool = esp_mcp_tool_create("self.device.set_time",
                               "Set device clock hour (0-23) and minute (0-59)", tool_set_time);
    if (tool != NULL) {
        prop = esp_mcp_property_create_with_range("hour", 0, 23);
        if (prop != NULL) {
            (void)esp_mcp_tool_add_property(tool, prop);
        }
        prop = esp_mcp_property_create_with_range("minute", 0, 59);
        if (prop != NULL) {
            (void)esp_mcp_tool_add_property(tool, prop);
        }
        (void)esp_mcp_add_tool(mcp, tool);
    }

    tool = esp_mcp_tool_create("self.device.show_message",
                               "Show a short text message on the device screen", tool_show_message);
    if (tool != NULL) {
        prop = esp_mcp_property_create_with_string("text", "hello");
        if (prop != NULL) {
            (void)esp_mcp_tool_add_property(tool, prop);
        }
        (void)esp_mcp_add_tool(mcp, tool);
    }

    ESP_LOGI(TAG, "已注册 5 个设备工具");
}
