#include "srv_audio.h"

#include "srv_config.h"
#include "srv_rtc.h"

#include "esp_log.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
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

/* 麦克风（录音机 / 语音助手共用） */
static esp_codec_dev_handle_t s_mic = NULL;
static bool                   s_mic_open = false;
/*! 麦克风增益（dB）。ES8311 的 ADC PGA 支持 0~30dB，模拟麦本体输出偏小，
 *  默认给 24dB；如果录音/语音上行出现削顶失真就往下调到 18 或 12 */
#define MIC_GAIN_DB  24.0f
/*! 麦克风单独一把锁：读麦会阻塞 100ms 级，不能和扬声器写共用一把，
 *  否则 TTS 播放会被录音的读操作卡住 */
static SemaphoreHandle_t      s_mic_mtx = NULL;

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
    if (s_mic_mtx == NULL) {
        s_mic_mtx = xSemaphoreCreateMutex();
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
    /* 必须持锁：电台拆成了取流（生产者）/写 I2S（消费者）两个任务，
     * 生产者可能在消费者正写着的时候调用 srv_audio_pcm_close()，
     * 不锁的话 esp_codec_dev_write 会打在已经关掉的设备上。
     * 阻塞的写过程持锁没关系：close 前会先把 primed 清掉让消费者收手。 */
    if (s_spk == NULL || data == NULL || bytes <= 0) {
        return -1;
    }

    dev_lock();
    int ret = -1;
    if (s_pcm_open) {
        ret = esp_codec_dev_write(s_spk, (void *)data, bytes);
    }
    dev_unlock();

    return ret;
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

/* ===================== 麦克风（录音机 / 语音助手共用） ===================== */

/**
 * @brief 建一个录音方向的 ES8311 设备（懒创建，两个使用者共用）
 *
 * BSP 头里虽然声明了 bsp_audio_codec_microphone_init()，但组件里只有 speaker
 * 的实现（链接时会报 undefined reference），所以这里照 speaker 的写法自己搭一个：
 * 复用 BSP 建好的 I2S（片上是全双工）和 I2C 总线，只新建一个 ADC 方向的 codec 设备。
 */
static esp_codec_dev_handle_t mic_dev_create(void)
{
    const audio_codec_data_if_t *data_if = bsp_audio_get_codec_itf();
    i2c_master_bus_handle_t      i2c_bus = bsp_i2c_get_handle();

    if (data_if == NULL || i2c_bus == NULL) {
        ESP_LOGE(TAG, "I2S/I2C 尚未就绪（音频要先初始化）");
        return NULL;
    }

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = BSP_I2C_NUM,
        .addr       = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == NULL) {
        ESP_LOGE(TAG, "codec 控制接口创建失败");
        return NULL;
    }

    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage        = 5.0,
        .codec_dac_voltage = 3.3,
    };

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if     = ctrl_if,
        .gpio_if     = gpio_if,
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_ADC,     /* 只用录音通路 */
        .pa_pin      = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk    = false,
        .digital_mic = false,                           /* 板上是模拟麦 */
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain     = gain,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (codec_if == NULL) {
        ESP_LOGE(TAG, "ES8311 codec 创建失败");
        return NULL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_if,
        .data_if  = data_if,
    };
    esp_codec_dev_handle_t dev = esp_codec_dev_new(&dev_cfg);
    if (dev == NULL) {
        ESP_LOGE(TAG, "录音设备创建失败");
    }
    return dev;
}

esp_err_t srv_audio_mic_open(int sample_rate, int channels, int bits)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_rate <= 0 || channels <= 0 || bits <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mic_open) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_mic_mtx == NULL) {
        s_mic_mtx = xSemaphoreCreateMutex();
    }
    if (s_mic == NULL) {
        s_mic = mic_dev_create();
        if (s_mic == NULL) {
            return ESP_FAIL;
        }
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = (uint8_t)bits,
        .channel         = (uint8_t)channels,
        .sample_rate     = (uint32_t)sample_rate,
    };

    if (s_mic_mtx != NULL) {
        xSemaphoreTake(s_mic_mtx, portMAX_DELAY);
    }
    esp_err_t ret = esp_codec_dev_open(s_mic, &fs);
    if (ret == ESP_OK) {
        s_mic_open = true;
        /* 打开之后再设增益：ES8311 的 PGA 是 codec 打开后才可写的 */
        esp_err_t gr = esp_codec_dev_set_in_gain(s_mic, MIC_GAIN_DB);
        if (gr != ESP_OK) {
            ESP_LOGW(TAG, "麦克风增益设置失败: %s", esp_err_to_name(gr));
        } else {
            ESP_LOGI(TAG, "麦克风增益 %.0f dB", (double)MIC_GAIN_DB);
        }
    }
    if (s_mic_mtx != NULL) {
        xSemaphoreGive(s_mic_mtx);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "麦克风打开失败: %s", esp_err_to_name(ret));
    }
    return ret;
}

int srv_audio_mic_read(void *data, int bytes)
{
    if (s_mic == NULL || data == NULL || bytes <= 0) {
        return -1;
    }

    int ret = -1;
    if (s_mic_mtx != NULL) {
        xSemaphoreTake(s_mic_mtx, portMAX_DELAY);
    }
    if (s_mic_open) {
        ret = esp_codec_dev_read(s_mic, data, bytes);
    }
    if (s_mic_mtx != NULL) {
        xSemaphoreGive(s_mic_mtx);
    }
    return ret;
}

void srv_audio_mic_close(void)
{
    if (!s_mic_open) {
        return;
    }
    if (s_mic_mtx != NULL) {
        xSemaphoreTake(s_mic_mtx, portMAX_DELAY);
    }
    s_mic_open = false;
    esp_codec_dev_close(s_mic);
    if (s_mic_mtx != NULL) {
        xSemaphoreGive(s_mic_mtx);
    }
}

bool srv_audio_mic_is_open(void)
{
    return s_mic_open;
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
