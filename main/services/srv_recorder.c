#include "srv_recorder.h"

#include "srv_sd.h"

#include "bsp/esp32_c5_touch_lcd_2.8.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "srv_recorder";

/*! SD 卡挂载点下的录音目录。
 *  写成编译期拼接的常量串（而不是运行时 snprintf）：格式化输出时才算得出
 *  最大长度，否则会撞上 -Werror=format-truncation */
#define REC_DIR_PATH    BSP_SD_MOUNT_POINT "/record"
/*! 16kHz 单声道 16bit：语音足够，一分钟约 1.9MB */
#define REC_SAMPLE_RATE 16000
/*! 单次最长时长：防止忘停把卡写满 */
#define REC_MAX_SEC     600
/*! 每次从 codec 读的字节数（2048 个采样） */
#define REC_BUF_BYTES   4096
#define WAV_HEADER_LEN  44

static esp_codec_dev_handle_t s_mic = NULL;
static bool                   s_inited = false;

static volatile bool s_running;
static volatile bool s_stop_req;
static int64_t       s_start_us;
static int64_t       s_elapsed_ms;      /*!< 停下来之后保留上一次的时长 */
static char          s_path[80];        /*!< 完整路径，如 /sdcard/record/rec-3.wav */

/* ===================== 工具 ===================== */

/**
 * @brief 扫描目录里已有的 rec-N.wav，返回下一个可用序号
 */
static int next_index(void)
{
    int max = 0;

    DIR *d = opendir(REC_DIR_PATH);
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            int n = 0;
            if (sscanf(e->d_name, "rec-%d.wav", &n) == 1 && n > max) {
                max = n;
            }
        }
        closedir(d);
    }
    return max + 1;
}

/* 标准 44 字节 PCM wav 头 */
static void wav_header(uint8_t *h, uint32_t data_bytes)
{
    const uint16_t channels    = 1;
    const uint16_t bits        = 16;
    const uint32_t rate        = REC_SAMPLE_RATE;
    const uint32_t byte_rate   = rate * channels * bits / 8;
    const uint16_t block_align = channels * bits / 8;
    const uint32_t riff_size   = 36 + data_bytes;
    const uint32_t fmt_len     = 16;
    const uint16_t format_pcm  = 1;

    memcpy(h, "RIFF", 4);
    memcpy(h + 4, &riff_size, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    memcpy(h + 16, &fmt_len, 4);
    memcpy(h + 20, &format_pcm, 2);
    memcpy(h + 22, &channels, 2);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &block_align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_bytes, 4);
}

/* ===================== 录音任务 ===================== */

static void rec_task(void *arg)
{
    (void)arg;

    FILE *fp = fopen(s_path, "wb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "无法创建 %s", s_path);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    /* 先写占位头，录完回填真实长度（头里有两个长度字段必须事后再改） */
    uint8_t header[WAV_HEADER_LEN] = {0};
    fwrite(header, 1, sizeof(header), fp);

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = 1,
        .sample_rate     = REC_SAMPLE_RATE,
    };
    if (esp_codec_dev_open(s_mic, &fs) != ESP_OK) {
        ESP_LOGE(TAG, "麦克风打开失败");
        fclose(fp);
        remove(s_path);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    uint8_t *buf = malloc(REC_BUF_BYTES);
    if (buf == NULL) {
        esp_codec_dev_close(s_mic);
        fclose(fp);
        remove(s_path);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    uint32_t data_bytes = 0;
    int      fail_cnt   = 0;
    int32_t  peak       = 0;        /*!< 录音期间的采样峰值，全 0 说明麦克风通路没数据 */
    const int64_t limit_us = (int64_t)REC_MAX_SEC * 1000 * 1000;

    while (!s_stop_req) {
        /* 注意 esp_codec_dev_read() 的返回值：成功是 ESP_CODEC_DEV_OK(0)、失败是负数，
         * **不是**读到的字节数（驱动里 bytes_read 被丢掉了，见 platform/audio_codec_data_i2s.c）。
         * 所以按固定块长写盘；读取失败时一个字节都不能写，否则会把 buffer 里的旧数据写进文件。
         * 读满这块（4096 字节 = 128ms）用不到超时（1000 ticks），正常都会成功。 */
        int ret = esp_codec_dev_read(s_mic, buf, REC_BUF_BYTES);
        if (ret == ESP_CODEC_DEV_OK) {
            fail_cnt = 0;

            const int16_t *smp = (const int16_t *)buf;
            for (int i = 0; i < REC_BUF_BYTES / 2; i++) {
                int32_t v = smp[i] < 0 ? -smp[i] : smp[i];
                if (v > peak) {
                    peak = v;
                }
            }

            if (fwrite(buf, 1, REC_BUF_BYTES, fp) != REC_BUF_BYTES) {
                ESP_LOGE(TAG, "写盘失败，提前停止");
                break;
            }
            data_bytes += REC_BUF_BYTES;
        } else {
            if (fail_cnt < 3) {
                ESP_LOGW(TAG, "读取失败: %d", ret);
            }
            fail_cnt++;
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        int64_t elapsed_us = esp_timer_get_time() - s_start_us;
        s_elapsed_ms = elapsed_us / 1000;

        if (elapsed_us >= limit_us) {
            ESP_LOGW(TAG, "已到单次最长 %d 秒，自动停止", REC_MAX_SEC);
            break;
        }
    }

    esp_codec_dev_close(s_mic);
    free(buf);

    wav_header(header, data_bytes);
    fseek(fp, 0, SEEK_SET);
    fwrite(header, 1, sizeof(header), fp);
    fclose(fp);

    ESP_LOGI(TAG, "录音结束：%s（%u 字节，%lld 毫秒，峰值 %ld）",
             s_path, (unsigned)data_bytes, (long long)s_elapsed_ms, (long)peak);

    s_running  = false;
    s_stop_req = false;
    vTaskDelete(NULL);
}

/* ===================== 对外接口 ===================== */

/**
 * @brief 建一个录音方向的 ES8311 设备
 *
 * BSP 头里虽然声明了 bsp_audio_codec_microphone_init()，但组件里只有 speaker
 * 的实现（链接时会报 undefined reference），所以这里照 speaker 的写法自己搭一个：
 * 复用 BSP 建好的 I2S（片上是全双工）和 I2C 总线，只新建一个 ADC 方向的 codec 设备。
 */
static esp_codec_dev_handle_t mic_create(void)
{
    const audio_codec_data_if_t *data_if = bsp_audio_get_codec_itf();
    i2c_master_bus_handle_t      i2c_bus = bsp_i2c_get_handle();

    if (data_if == NULL || i2c_bus == NULL) {
        ESP_LOGE(TAG, "I2S/I2C 尚未就绪（srv_audio_init 要先跑）");
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

esp_err_t srv_recorder_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    s_mic = mic_create();
    if (s_mic == NULL) {
        return ESP_FAIL;
    }

    s_inited = true;
    ESP_LOGI(TAG, "录音服务就绪（%dHz 单声道，输出 %s/rec-N.wav）",
             REC_SAMPLE_RATE, REC_DIR_PATH);
    return ESP_OK;
}

esp_err_t srv_recorder_start(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!srv_sd_is_mounted()) {
        ESP_LOGE(TAG, "SD 未挂载，无法录音");
        return ESP_ERR_INVALID_STATE;
    }

    /* 首次录音时目录还不存在 */
    struct stat st;
    if (stat(REC_DIR_PATH, &st) != 0) {
        if (mkdir(REC_DIR_PATH, 0777) != 0) {
            ESP_LOGE(TAG, "创建目录失败: %s", REC_DIR_PATH);
            return ESP_FAIL;
        }
    }

    snprintf(s_path, sizeof(s_path), REC_DIR_PATH "/rec-%d.wav", next_index());

    s_elapsed_ms = 0;
    s_stop_req   = false;
    s_start_us   = esp_timer_get_time();
    s_running    = true;

    if (xTaskCreate(rec_task, "recorder", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "录音任务创建失败");
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "开始录音 -> %s", s_path);
    return ESP_OK;
}

esp_err_t srv_recorder_stop(void)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    s_stop_req = true;

    /* 等任务把 wav 头写好、文件关掉再返回：上面那层拿到的文件必须是完整的 */
    for (int i = 0; i < 300 && s_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_running) {
        ESP_LOGW(TAG, "录音任务收尾超时");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

bool srv_recorder_is_active(void)
{
    return s_running;
}

int64_t srv_recorder_elapsed_ms(void)
{
    if (!s_running) {
        return s_elapsed_ms;
    }
    return (esp_timer_get_time() - s_start_us) / 1000;
}

const char *srv_recorder_last_file(void)
{
    return s_path;
}
