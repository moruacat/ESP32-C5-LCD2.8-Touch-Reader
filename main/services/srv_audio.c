#include "srv_audio.h"

#include "srv_config.h"
#include "srv_rtc.h"

#include "esp_log.h"
#include "esp_codec_dev.h"
#include "bsp/esp32_c5_touch_lcd_2.8.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "srv_audio";

#define BEEP_SAMPLE_RATE   16000
#define BEEP_MAX_MS        5000

static esp_codec_dev_handle_t s_spk = NULL;
static bool  s_inited = false;
static int   s_volume = 0;

/* 保护扬声器通道的打开/关闭：音乐播放与提示音互斥 */
static SemaphoreHandle_t s_dev_mtx = NULL;
static bool  s_pcm_open = false;

static void dev_lock(void)
{
    if (s_dev_mtx != NULL) {
        xSemaphoreTake(s_dev_mtx, portMAX_DELAY);
    }
}

static void dev_unlock(void)
{
    if (s_dev_mtx != NULL) {
        xSemaphoreGive(s_dev_mtx);
    }
}

esp_err_t srv_audio_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    if (s_dev_mtx == NULL) {
        s_dev_mtx = xSemaphoreCreateMutex();
    }

    /* i2s_config 传 NULL 时 BSP 使用内置默认（22050Hz / 单声道 / 16bit） */
    esp_err_t ret = bsp_audio_init(NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    s_spk = bsp_audio_codec_speaker_init();
    if (s_spk == NULL) {
        ESP_LOGE(TAG, "扬声器 codec 初始化失败");
        return ESP_FAIL;
    }

    s_volume = (int)srv_config_get_int(CFG_VOLUME, 60);
    esp_codec_dev_set_out_vol(s_spk, s_volume);
    esp_codec_dev_set_out_mute(s_spk, false);

    s_inited = true;
    ESP_LOGI(TAG, "音频就绪（音量 %d%%，闹钟音量 %d%%）",
             s_volume, (int)srv_config_get_int(CFG_ALARM_VOLUME, CFG_DEF_ALARM_VOLUME));
    return ESP_OK;
}

/* ===================== 音量 ===================== */

esp_err_t srv_audio_set_volume(int vol)
{
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;

    s_volume = vol;
    if (s_spk) {
        esp_codec_dev_set_out_vol(s_spk, vol);
    }
    srv_config_set_int(CFG_VOLUME, vol, false);
    srv_config_save_deferred();
    ESP_LOGI(TAG, "音量 %d%%", vol);
    return ESP_OK;
}

int srv_audio_get_volume(void)
{
    return s_volume;
}

/* ===================== 闹钟音量 ===================== */

esp_err_t srv_audio_set_alarm_volume(int vol)
{
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;
    srv_config_set_int(CFG_ALARM_VOLUME, vol, false);
    srv_config_save_deferred();
    return ESP_OK;
}

int srv_audio_get_alarm_volume(void)
{
    return (int)srv_config_get_int(CFG_ALARM_VOLUME, CFG_DEF_ALARM_VOLUME);
}

/* ===================== 音乐时段静音 ===================== */

bool srv_audio_in_mute_window(void)
{
    srv_rtc_time_t t;
    if (srv_rtc_get_time(&t) != ESP_OK) {
        return false;
    }
    int now = t.hour * 60 + t.min;

    for (int i = 0; i < SRV_CONFIG_MUTE_NUM; i++) {
        char key[16];
        snprintf(key, sizeof(key), "mute%d_en", i + 1);
        if (srv_config_get_int(key, 0) == 0) {
            continue;
        }
        snprintf(key, sizeof(key), "mute%d_s", i + 1);
        int start = (int)srv_config_get_int(key, 0);
        snprintf(key, sizeof(key), "mute%d_e", i + 1);
        int end = (int)srv_config_get_int(key, 0);

        if (start == end) {
            continue;                       /* 空时段 */
        }
        bool inside = (start < end) ? (now >= start && now < end)
                                    : (now >= start || now < end);   /* 跨零点 */
        if (inside) {
            return true;
        }
    }
    return false;
}

bool srv_audio_apply_time_mute(void)
{
    /* 只在「进音乐页」时调一次。落在时段内就把音乐音量压到 0% 并落盘，
     * 之后不管了 —— 不再每分钟跟着时段变，暂停/继续/切歌也不碰音量。
     * 音量已经就是 0 时直接返回，免得白写一次 SD。 */
    if (!srv_audio_in_mute_window() || s_volume == 0) {
        return false;
    }

    ESP_LOGI(TAG, "在静音时段内进入音乐，音量压到 0%%（原来 %d%%）", s_volume);
    srv_audio_set_volume(0);
    return true;
}

/* ===================== PCM 播放（音乐播放器用） ===================== */

esp_err_t srv_audio_pcm_open(int sample_rate, int channels, int bits)
{
    if (!s_inited || s_spk == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_rate <= 0 || channels <= 0 || bits <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = (uint8_t)bits,
        .channel         = (uint8_t)channels,
        .sample_rate     = (uint32_t)sample_rate,
    };

    dev_lock();
    esp_err_t ret = esp_codec_dev_open(s_spk, &fs);
    if (ret == ESP_OK) {
        s_pcm_open = true;
    }
    dev_unlock();

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "音乐通道打开失败: %s", esp_err_to_name(ret));
    }
    return ret;
}

int srv_audio_pcm_write(const void *data, int bytes)
{
    if (!s_pcm_open || s_spk == NULL || data == NULL || bytes <= 0) {
        return -1;
    }
    return esp_codec_dev_write(s_spk, (void *)data, bytes);
}

void srv_audio_pcm_close(void)
{
    if (!s_pcm_open) {
        return;
    }
    dev_lock();
    s_pcm_open = false;
    esp_codec_dev_close(s_spk);
    dev_unlock();
}

bool srv_audio_pcm_is_open(void)
{
    return s_pcm_open;
}

/* ===================== 提示音 ===================== */

esp_err_t srv_audio_beep(int freq_hz, int duration_ms)
{
    if (!s_inited || s_spk == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (freq_hz <= 0 || duration_ms <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (duration_ms > BEEP_MAX_MS) {
        duration_ms = BEEP_MAX_MS;
    }

    /* 音乐播放器占着通道时不抢：上层需先停音乐再响铃 */
    if (s_pcm_open) {
        ESP_LOGW(TAG, "音乐正在播放，提示音跳过");
        return ESP_ERR_INVALID_STATE;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = 1,
        .sample_rate     = BEEP_SAMPLE_RATE,
    };

    dev_lock();
    esp_err_t ret = esp_codec_dev_open(s_spk, &fs);
    if (ret != ESP_OK) {
        dev_unlock();
        ESP_LOGE(TAG, "codec 打开失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 半周期采样数，方波 */
    const int half = BEEP_SAMPLE_RATE / (freq_hz * 2);
    const int total = BEEP_SAMPLE_RATE * duration_ms / 1000;
    const int chunk = 256;

    int16_t *buf = malloc(chunk * sizeof(int16_t));
    if (buf == NULL) {
        esp_codec_dev_close(s_spk);
        dev_unlock();
        return ESP_ERR_NO_MEM;
    }

    int written = 0;
    while (written < total) {
        int n = (total - written < chunk) ? (total - written) : chunk;
        for (int i = 0; i < n; i++) {
            int phase = (written + i) / (half > 0 ? half : 1);
            buf[i] = (phase & 1) ? -8000 : 8000;
        }
        esp_codec_dev_write(s_spk, buf, n * sizeof(int16_t));
        written += n;
    }

    free(buf);
    esp_codec_dev_close(s_spk);
    dev_unlock();
    return ESP_OK;
}
