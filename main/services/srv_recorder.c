#include "srv_recorder.h"

#include "srv_sd.h"
#include "srv_audio.h"

#include "bsp/esp32_c5_touch_lcd_2.8.h"
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
/*! 单文件最长时长：到点自动切下一个文件继续录（一小时约 110MB） */
#define REC_MAX_SEC     3600
/*! 每次从 codec 读的字节数（2048 个采样） */
#define REC_BUF_BYTES   4096
#define WAV_HEADER_LEN  44

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

/*! 当前文件的序号：满一小时切文件时 +1 */
static int s_file_idx;

/**
 * @brief 打开下一个录音文件并写占位头
 * @return 文件句柄；失败返回 NULL
 */
static FILE *rec_file_open(int idx)
{
    snprintf(s_path, sizeof(s_path), REC_DIR_PATH "/rec-%d.wav", idx);

    FILE *fp = fopen(s_path, "wb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "无法创建 %s", s_path);
        return NULL;
    }

    /* 先写占位头，录完回填真实长度（头里有两个长度字段必须事后再改） */
    uint8_t header[WAV_HEADER_LEN] = {0};
    fwrite(header, 1, sizeof(header), fp);
    return fp;
}

/**
 * @brief 收尾当前文件：回填 wav 头长度并关文件
 */
static void rec_file_close(FILE *fp, uint32_t data_bytes)
{
    uint8_t header[WAV_HEADER_LEN] = {0};
    wav_header(header, data_bytes);
    fseek(fp, 0, SEEK_SET);
    fwrite(header, 1, sizeof(header), fp);
    fclose(fp);
}

static void rec_task(void *arg)
{
    (void)arg;

    FILE *fp = rec_file_open(s_file_idx);
    if (fp == NULL) {
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    if (srv_audio_mic_open(REC_SAMPLE_RATE, 1, 16) != ESP_OK) {
        ESP_LOGE(TAG, "麦克风打开失败（可能小智正在占用）");
        fclose(fp);
        remove(s_path);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    uint8_t *buf = malloc(REC_BUF_BYTES);
    if (buf == NULL) {
        srv_audio_mic_close();
        fclose(fp);
        remove(s_path);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    uint32_t data_bytes = 0;        /*!< 当前文件的 PCM 字节数 */
    int      fail_cnt   = 0;
    int32_t  peak       = 0;        /*!< 录音期间的采样峰值，全 0 说明麦克风通路没数据 */
    int      file_cnt   = 1;        /*!< 本次录音一共写了几个文件 */
    const int64_t limit_us = (int64_t)REC_MAX_SEC * 1000 * 1000;
    /* 单文件计时与总时长分开：s_start_us 是整次录音的起点（UI 计时用），
     * 切文件只重置 file_start，界面上的计时才不会被"归零" */
    int64_t file_start_us = s_start_us;

    while (!s_stop_req) {
        /* 注意 esp_codec_dev_read() 的返回值：成功是 ESP_CODEC_DEV_OK(0)、失败是负数，
         * **不是**读到的字节数（驱动里 bytes_read 被丢掉了，见 platform/audio_codec_data_i2s.c）。
         * 所以按固定块长写盘；读取失败时一个字节都不能写，否则会把 buffer 里的旧数据写进文件。
         * 读满这块（4096 字节 = 128ms）用不到超时（1000 ticks），正常都会成功。 */
        int ret = srv_audio_mic_read(buf, REC_BUF_BYTES);
        if (ret == 0) {
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

        s_elapsed_ms = (esp_timer_get_time() - s_start_us) / 1000;

        /* 单文件到点：把这一个收尾，接着录下一个（循环录制，不断音） */
        if ((esp_timer_get_time() - file_start_us) >= limit_us) {
            ESP_LOGI(TAG, "单文件已满 %d 秒，切到下一个文件", REC_MAX_SEC);
            rec_file_close(fp, data_bytes);
            data_bytes = 0;
            file_cnt++;
            file_start_us = esp_timer_get_time();

            fp = rec_file_open(s_file_idx + file_cnt - 1);
            if (fp == NULL) {
                s_running = false;
                break;
            }
        }
    }

    srv_audio_mic_close();
    free(buf);

    rec_file_close(fp, data_bytes);

    ESP_LOGI(TAG, "录音结束：%d 个文件，最后 %s（%u 字节，共 %lld 毫秒，峰值 %ld）",
             file_cnt, s_path, (unsigned)data_bytes,
             (long long)s_elapsed_ms, (long)peak);

    s_running  = false;
    s_stop_req = false;
    vTaskDelete(NULL);
}

/* ===================== 对外接口 ===================== */

/* ===================== 对外接口 ===================== */

esp_err_t srv_recorder_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    /* 麦克风是 srv_audio 里的共用单例（录音机 / 小智语音助手），
     * 这里只做可用性检查，真正打开在每次录音任务里 */
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

    s_file_idx = next_index();
    snprintf(s_path, sizeof(s_path), REC_DIR_PATH "/rec-%d.wav", s_file_idx);

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
