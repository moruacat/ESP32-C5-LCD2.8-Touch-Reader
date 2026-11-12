#include "srv_player.h"

#include "srv_audio.h"
#include "srv_musiclib.h"
#include "srv_progress.h"
#include "srv_sd.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_dec_default.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "srv_player";

/* 输入缓冲加大到 64KB：SD 与 LCD 共用 SPI2，LVGL 刷屏时 fread 会被总线锁住；
 * 原来 8KB 只够约 20ms 音频，一被阻塞就断音。64KB 一次读得多、被阻塞的次数少。
 * PCM 缓冲同步加大，减少 I2S 写入次数。两块都在 PSRAM（big_malloc）。 */
#define IN_BUF_BYTES        65536
#define PCM_BUF_BYTES       32768
/* 整首预读上限：SD 与 LCD 共用 SPI2，只要播放期间还在读卡就一定有细微卡顿。
 * 小文件整首读进 PSRAM 后，播放阶段零 SD 访问；超过上限的回退流式。 */
#define PRELOAD_MAX         (4 * 1024 * 1024)
#define PCM_BUF_MAX         (64 * 1024)
#define CMD_Q_LEN           8
#define PLAYER_TASK_STACK   6144
/* 单核 C5：播放任务必须比 LVGL / 触摸高，否则一次 I2C 事务或刷屏就能把
 * PCM 供数拖过 DMA 缓冲长度，听感就是"每隔一会儿顿一下" */
#define PLAYER_TASK_PRIO    10
#define SAVE_MIN_INTERVAL_US (5 * 1000 * 1000)
#define EMPTY_BLOCK_GUARD   32      /* 连续无输出时的保护次数 */

enum {
    CMD_PLAY = 1,
    CMD_PAUSE,
    CMD_RESUME,
    CMD_STOP,
    CMD_NEXT,
    CMD_PREV,
    CMD_SEEK,
};

typedef struct {
    int32_t cmd;
    int32_t arg;
} player_cmd_t;

/**
 * @brief 当前曲目的播放上下文（只在播放任务里使用）
 */
typedef struct {
    FILE                         *fp;
    esp_audio_simple_dec_handle_t dec;
    uint8_t                      *in_buf;
    int                           in_len;
    int                           in_used;
    bool                          eos;
    /* 整首预读：非空时 track_fill 从这块 PSRAM 取数据，fp 已经关闭 */
    uint8_t                      *preload;
    uint32_t                      preload_size;
    uint32_t                      preload_pos;
    uint8_t                      *pcm_buf;
    int                           pcm_cap;
    int                           sample_rate;
    int                           channels;
    int                           bits;
    int64_t                       pos_samples;
    uint32_t                      data_off;
    uint32_t                      file_size;
    int32_t                       xing_dur_ms;   /*!< Xing/Info 头算出的时长；0 = 没有，退回码率估算 */
    srv_music_fmt_t               fmt;
    bool                          codec_open;
} track_t;

static volatile srv_player_state_t s_state = SRV_PLAYER_IDLE;
static volatile int32_t s_pos_ms = 0;
static volatile int32_t s_dur_ms = 0;
static volatile int32_t s_index  = -1;

static srv_player_mode_t s_mode    = SRV_PLAYER_MODE_LIST_LOOP;
static int               s_list_id = 0;

static TaskHandle_t  s_task = NULL;
static QueueHandle_t s_q    = NULL;

static int64_t s_last_save_us = 0;

/* ===================== 总线占用统计（串口排查卡顿） =====================
 * 播放卡顿的两个来源都和总线有关：SD 读卡（SPI2，与 LCD 共用）和 I2S 写。
 * 每 5 秒输出一次窗口内的累计耗时 / 单次峰值 / 占音频时长的比例：
 *  - 读占比高  -> 流式播放时 SD 被刷屏抢总线，需要更大的预读缓冲
 *  - 写占比接近 100% -> I2S 已经喂满，卡顿来自解码太慢
 *  - 峰值大但占比低 -> 偶发长阻塞（刷屏、WiFi、其他 SD 访问） */
#define BUS_STAT_WINDOW_US  (5 * 1000 * 1000)

static int64_t s_bus_t0        = 0;
static int64_t s_bus_read_us   = 0;
static int64_t s_bus_read_max  = 0;
static int64_t s_bus_write_us  = 0;
static int64_t s_bus_write_max = 0;
static int     s_bus_frames    = 0;

static void bus_stat_add(int64_t read_us, int64_t write_us,
                         int bytes, int sample_rate, int frame)
{
    const int64_t now = esp_timer_get_time();
    if (s_bus_t0 == 0) {
        s_bus_t0 = now;
    }

    if (read_us > 0) {
        s_bus_read_us += read_us;
        if (read_us > s_bus_read_max) {
            s_bus_read_max = read_us;
        }
    }
    if (write_us > 0) {
        s_bus_write_us += write_us;
        if (write_us > s_bus_write_max) {
            s_bus_write_max = write_us;
        }
    }

    /* 这个窗口送出去的 PCM 对应多长的音频 */
    if (frame > 0 && sample_rate > 0 && bytes > 0) {
        s_bus_frames += bytes / frame;
    }

    const int64_t span = now - s_bus_t0;
    if (span < BUS_STAT_WINDOW_US) {
        return;
    }

    const int64_t audio_us = (sample_rate > 0 && frame > 0)
                           ? s_bus_frames * 1000000 / sample_rate : 0;
    /* 占用率用千分比算，串口日志不依赖浮点格式化 */
    int rd_pm = (audio_us > 0) ? (int)(s_bus_read_us  * 1000 / audio_us) : 0;
    int wr_pm = (audio_us > 0) ? (int)(s_bus_write_us * 1000 / audio_us) : 0;

    ESP_LOGI(TAG, "[bus] %llds | 音频 %lld ms | 读卡 %lld ms(峰 %lld) | 写 I2S %lld ms(峰 %lld) | 占用 读 %d.%d%% 写 %d.%d%%",
             (long long)(span / 1000000),
             (long long)(audio_us / 1000),
             (long long)(s_bus_read_us / 1000),  (long long)(s_bus_read_max / 1000),
             (long long)(s_bus_write_us / 1000), (long long)(s_bus_write_max / 1000),
             rd_pm / 10, rd_pm % 10, wr_pm / 10, wr_pm % 10);

    s_bus_t0        = now;
    s_bus_read_us   = 0;
    s_bus_read_max  = 0;
    s_bus_write_us  = 0;
    s_bus_write_max = 0;
    s_bus_frames    = 0;
}

/* 换曲/停止时重置统计窗口，免得下首歌的第一行日志混进上一首的空档 */
static void bus_stat_reset(void)
{
    s_bus_t0        = 0;
    s_bus_read_us   = 0;
    s_bus_read_max  = 0;
    s_bus_write_us  = 0;
    s_bus_write_max = 0;
    s_bus_frames    = 0;
}

/* ===================== 队列 ===================== */

static bool queue_is_library(void)
{
    return s_mode == SRV_PLAYER_MODE_ALL_LOOP;
}

static int queue_count(void)
{
    if (!srv_sd_is_mounted()) {
        return 0;
    }
    if (queue_is_library()) {
        return srv_musiclib_count();
    }
    return srv_musiclib_playlist_item_count();
}

static const srv_music_t *queue_item(int idx)
{
    if (idx < 0) {
        return NULL;
    }
    const srv_music_t *m = NULL;
    if (queue_is_library()) {
        if (srv_musiclib_get(idx, &m) != ESP_OK) {
            return NULL;
        }
    } else {
        if (srv_musiclib_playlist_item(idx, &m) != ESP_OK) {
            return NULL;
        }
    }
    return m;
}

/* ===================== 小工具 ===================== */

static void post_cmd(int32_t cmd, int32_t arg)
{
    if (s_q == NULL) {
        return;
    }
    player_cmd_t c = { .cmd = cmd, .arg = arg };
    (void)xQueueSend(s_q, &c, pdMS_TO_TICKS(200));
}

static void *big_malloc(size_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = malloc(bytes);
    }
    return p;
}

static esp_audio_simple_dec_type_t dec_type_of(srv_music_fmt_t fmt)
{
    switch (fmt) {
    case SRV_MUSIC_FMT_MP3:  return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    case SRV_MUSIC_FMT_WAV:  return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    case SRV_MUSIC_FMT_AAC:  return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    case SRV_MUSIC_FMT_M4A:  return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    case SRV_MUSIC_FMT_FLAC: return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    case SRV_MUSIC_FMT_OGG:  return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
    case SRV_MUSIC_FMT_AMR:  return ESP_AUDIO_SIMPLE_DEC_TYPE_AMRNB;
    case SRV_MUSIC_FMT_TS:   return ESP_AUDIO_SIMPLE_DEC_TYPE_TS;
    default:                 return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    }
}

/* 解析 WAV，返回 data 块起始偏移（0 = 不是合法 WAV）；顺带取格式参数 */
static uint32_t wav_parse(FILE *fp, int *sample_rate, int *channels, int *bits)
{
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, fp) != 12) {
        return 0;
    }
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        return 0;
    }

    uint32_t off = 12;
    while (off < 4096) {
        uint8_t ck[8];
        if (fread(ck, 1, 8, fp) != 8) {
            break;
        }
        uint32_t csize = (uint32_t)ck[4] | ((uint32_t)ck[5] << 8) |
                         ((uint32_t)ck[6] << 16) | ((uint32_t)ck[7] << 24);
        uint32_t step = 8 + csize + (csize & 1u);

        if (memcmp(ck, "fmt ", 4) == 0 && csize >= 16) {
            uint8_t f[16];
            if (fread(f, 1, 16, fp) != 16) {
                break;
            }
            *channels    = (int)((uint32_t)f[2] | ((uint32_t)f[3] << 8));
            *sample_rate = (int)((uint32_t)f[4] | ((uint32_t)f[5] << 8) |
                                 ((uint32_t)f[6] << 16) | ((uint32_t)f[7] << 24));
            *bits        = (int)((uint32_t)f[14] | ((uint32_t)f[15] << 8));
            if (csize > 16) {
                fseek(fp, (long)(csize - 16 + (csize & 1u)), SEEK_CUR);
            }
        } else if (memcmp(ck, "data", 4) == 0) {
            return off + 8;
        } else {
            fseek(fp, (long)(csize + (csize & 1u)), SEEK_CUR);
        }
        off += step;
    }
    return 0;
}

/* 从 pos 起往后找 MP3 帧同步字（找不到就返回原位置） */
static uint32_t mp3_find_sync(FILE *fp, uint32_t pos, uint32_t end)
{
    if (pos >= end) {
        return pos;
    }
    uint8_t buf[1024];
    fseek(fp, (long)pos, SEEK_SET);
    size_t n = fread(buf, 1, sizeof(buf), fp);

    for (size_t i = 0; i + 1 < n; i++) {
        if (buf[i] == 0xFF && (buf[i + 1] & 0xE0) == 0xE0) {
            return pos + (uint32_t)i;
        }
    }
    return pos;
}

/* ===================== MP3 帧头 / Xing ===================== */

#define MP3_SCAN_BYTES  16384   /* 找首帧时最多往后扫这么多字节 */

typedef struct {
    int      sample_rate;
    int      channels;
    int      samples_per_frame;
    uint32_t frame_len;
    bool     mpeg1;
} mp3_frame_t;

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* 校验并解析一个 Layer III 帧头；不是合法帧返回 false */
static bool mp3_parse_frame(const uint8_t *p, size_t avail, mp3_frame_t *out)
{
    static const uint16_t br_v1[16] = { 0, 32, 40, 48, 56, 64, 80, 96,
                                        112, 128, 160, 192, 224, 256, 320, 0 };
    static const uint16_t br_v2[16] = { 0, 8, 16, 24, 32, 40, 48, 56,
                                        64, 80, 96, 112, 128, 144, 160, 0 };
    static const uint16_t sr_tab[3][3] = {
        { 44100, 48000, 32000 },    /* MPEG1 */
        { 22050, 24000, 16000 },    /* MPEG2 */
        { 11025, 12000,  8000 },    /* MPEG2.5 */
    };

    if (avail < 4 || p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) {
        return false;
    }

    int ver   = (p[1] >> 3) & 3;    /* 3=MPEG1 2=MPEG2 0=MPEG2.5 1=保留 */
    int layer = (p[1] >> 1) & 3;    /* 1=Layer III */
    int br_i  = (p[2] >> 4) & 0xF;
    int sr_i  = (p[2] >> 2) & 3;
    int pad   = (p[2] >> 1) & 1;
    int chn   = (p[3] >> 6) & 3;

    if (ver == 1 || layer != 1 || br_i == 0 || br_i == 0xF || sr_i == 3) {
        return false;
    }

    int vi   = (ver == 3) ? 0 : ((ver == 2) ? 1 : 2);
    int kbps = (ver == 3) ? br_v1[br_i] : br_v2[br_i];
    int sr   = sr_tab[vi][sr_i];
    int spf  = (ver == 3) ? 1152 : 576;

    out->sample_rate       = sr;
    out->channels          = (chn == 3) ? 1 : 2;
    out->samples_per_frame = spf;
    out->mpeg1             = (ver == 3);
    out->frame_len         = (uint32_t)(spf / 8 * kbps * 1000 / sr) + (uint32_t)pad;
    return out->frame_len >= 4;
}

/* ID3v2 标签总长度（含 10 字节头）；没有标签返回 0 */
static uint32_t id3v2_size(FILE *fp)
{
    uint8_t h[10];
    fseek(fp, 0L, SEEK_SET);
    if (fread(h, 1, 10, fp) != 10) {
        return 0;
    }
    if (memcmp(h, "ID3", 3) != 0 || h[3] == 0xFF || h[4] == 0xFF) {
        return 0;
    }
    uint32_t sz = ((uint32_t)(h[6] & 0x7F) << 21) | ((uint32_t)(h[7] & 0x7F) << 14) |
                  ((uint32_t)(h[8] & 0x7F) << 7) | (uint32_t)(h[9] & 0x7F);
    /* v2.4 带 footer 时总长还要多算 10 字节 */
    return 10 + sz + ((h[5] & 0x10) ? 10 : 0);
}

/* 在 [start, end) 里找第一个合法音频帧的偏移；找不到返回 0 */
static uint32_t mp3_first_frame(FILE *fp, uint32_t start, uint32_t end)
{
    if (start >= end) {
        return 0;
    }
    uint32_t limit = start + MP3_SCAN_BYTES;
    if (limit > end) {
        limit = end;
    }

    size_t want = (size_t)(limit - start);
    uint8_t *buf = big_malloc(want);
    if (buf == NULL) {
        return 0;
    }
    fseek(fp, (long)start, SEEK_SET);
    size_t n = fread(buf, 1, want, fp);

    uint32_t off = 0;
    for (size_t i = 0; i + 4 <= n; i++) {
        mp3_frame_t f;
        if (!mp3_parse_frame(buf + i, n - i, &f)) {
            continue;
        }
        /* 用下一帧再验一次，避免把标签/音频数据里的 0xFF 误当帧头 */
        size_t nx = i + f.frame_len;
        if (nx + 4 > n) {
            continue;               /* 缓冲区边界，读不到下一帧就不采信 */
        }
        mp3_frame_t f2;
        if (!mp3_parse_frame(buf + nx, n - nx, &f2)) {
            continue;
        }
        off = start + (uint32_t)i;
        break;
    }
    free(buf);
    return off;
}

/* 首帧里的 Xing/Info 头带总帧数 → 精确时长；没有返回 0 */
static int32_t mp3_xing_dur_ms(FILE *fp, uint32_t frame_off, const mp3_frame_t *f)
{
    /* 侧信息长度：MPEG1 单声道 17、其余 32；MPEG2 单声道 9、其余 17 */
    uint32_t side = f->mpeg1 ? (f->channels == 1 ? 17u : 32u)
                             : (f->channels == 1 ? 9u : 17u);

    uint8_t hdr[12];
    fseek(fp, (long)(frame_off + 4 + side), SEEK_SET);
    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) {
        return 0;
    }
    if (memcmp(hdr, "Xing", 4) != 0 && memcmp(hdr, "Info", 4) != 0) {
        return 0;
    }
    if ((be32(hdr + 4) & 0x1u) == 0) {
        return 0;                   /* 没有帧数字段 */
    }

    uint32_t frames = be32(hdr + 8);
    if (frames == 0) {
        return 0;
    }
    int64_t ms = (int64_t)frames * f->samples_per_frame * 1000 / f->sample_rate;
    return (ms > 0 && ms < 24LL * 3600 * 1000) ? (int32_t)ms : 0;
}

/**
 * @brief 定位 MP3 第一个音频帧，并尽量取到精确时长
 * @param out_dur_ms 拿到 Xing/Info 总帧数时填精确时长，否则填 0
 * @return 首帧偏移；没找到返回 0（此时按整文件处理）
 */
static uint32_t mp3_probe(FILE *fp, uint32_t file_size, int32_t *out_dur_ms)
{
    *out_dur_ms = 0;

    uint32_t start = id3v2_size(fp);
    if (start >= file_size) {
        start = 0;
    }
    uint32_t off = mp3_first_frame(fp, start, file_size);
    if (off == 0) {
        return 0;
    }

    mp3_frame_t f;
    uint8_t h[4];
    fseek(fp, (long)off, SEEK_SET);
    if (fread(h, 1, 4, fp) == 4 && mp3_parse_frame(h, 4, &f)) {
        *out_dur_ms = mp3_xing_dur_ms(fp, off, &f);
    }
    return off;
}

/* ===================== 曲目上下文 ===================== */

static bool track_fill(track_t *t)
{
    /* 预读模式：直接从 PSRAM 拷贝，播放期间不再读 SD */
    if (t->preload != NULL) {
        uint32_t remain = t->preload_size - t->preload_pos;
        int n = (remain < (uint32_t)IN_BUF_BYTES) ? (int)remain : IN_BUF_BYTES;
        memcpy(t->in_buf, t->preload + t->preload_pos, (size_t)n);
        t->preload_pos += (uint32_t)n;
        t->in_len  = n;
        t->in_used = 0;
        t->eos     = (n < IN_BUF_BYTES);
        return n > 0;
    }

    const int64_t t0 = esp_timer_get_time();
    t->in_len = (int)fread(t->in_buf, 1, IN_BUF_BYTES, t->fp);
    const int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
    /* 每次读卡的耗时：超过 50ms 说明被 SPI 总线/LVGL 刷屏卡住了，会直接体现在这里 */
    if (dt_ms > 50) {
        ESP_LOGW(TAG, "读卡 %lld ms（%d 字节）", (long long)dt_ms, t->in_len);
    }
    bus_stat_add(dt_ms * 1000, 0, 0, 0, 0);
    t->in_used = 0;
    t->eos = (t->in_len < IN_BUF_BYTES);
    return t->in_len > 0;
}

static bool pcm_grow(track_t *t, uint32_t need)
{
    if (need == 0 || need > PCM_BUF_MAX) {
        return false;
    }
    uint8_t *nb = big_malloc(need);
    if (nb == NULL) {
        return false;
    }
    free(t->pcm_buf);
    t->pcm_buf = nb;
    t->pcm_cap = (int)need;
    return true;
}

/* 已解出的 PCM 送到扬声器并累计播放位置 */
static void track_emit(track_t *t, uint32_t bytes)
{
    const int64_t t0 = esp_timer_get_time();
    srv_audio_pcm_write(t->pcm_buf, (int)bytes);
    const int64_t write_us = esp_timer_get_time() - t0;

    bus_stat_add(0, write_us, (int)bytes, t->sample_rate, t->channels * (t->bits / 8));

    int frame = t->channels * (t->bits / 8);
    if (frame > 0 && t->sample_rate > 0) {
        t->pos_samples += (int64_t)(bytes / (uint32_t)frame);
        s_pos_ms = (int32_t)(t->pos_samples * 1000 / t->sample_rate);
    }
}

/**
 * @brief 解一帧并输出
 * @return 1 继续；0 本曲结束；-1 出错
 */
static int track_step(track_t *t)
{
    int empty_guard = 0;

    for (;;) {
        if (t->in_used >= t->in_len) {
            if (t->eos) {
                return 0;
            }
            if (!track_fill(t)) {
                t->eos = true;
                return 0;
            }
        }

        esp_audio_simple_dec_raw_t raw = {
            .buffer   = t->in_buf + t->in_used,
            .len      = (uint32_t)(t->in_len - t->in_used),
            .eos      = t->eos,
            .consumed = 0,
        };
        esp_audio_simple_dec_out_t out = {
            .buffer = t->pcm_buf,
            .len    = (uint32_t)t->pcm_cap,
        };

        esp_audio_err_t r = esp_audio_simple_dec_process(t->dec, &raw, &out);

        if (r == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            if (!pcm_grow(t, out.needed_size)) {
                ESP_LOGE(TAG, "PCM 缓冲扩容失败（need %u）", (unsigned)out.needed_size);
                return -1;
            }
            continue;
        }

        t->in_used += (int)raw.consumed;

        if (r != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "解码出错 %d，跳过当前数据块", (int)r);
            t->in_used = t->in_len;      /* 丢弃本块剩余，避免卡死 */
            return -1;
        }

        if (out.decoded_size == 0) {
            /* 输入被吃掉但没解出整帧：继续喂；真卡住就丢块推进 */
            if (raw.consumed == 0) {
                if (++empty_guard > EMPTY_BLOCK_GUARD) {
                    t->in_used = t->in_len;
                    empty_guard = 0;
                }
            }
            continue;
        }

        /* 输出前先把音频通道按真实格式打开 */
        if (!t->codec_open) {
            esp_err_t ar = srv_audio_pcm_open(t->sample_rate, t->channels, t->bits);
            if (ar != ESP_OK) {
                return -1;
            }
            t->codec_open = true;
        }

        track_emit(t, out.decoded_size);
        return 1;
    }
}

static void track_seek(track_t *t, int32_t ms)
{
    if (t->sample_rate <= 0 || t->file_size <= t->data_off) {
        return;
    }
    if (ms < 0) {
        ms = 0;
    }
    if (s_dur_ms > 0 && ms > s_dur_ms) {
        ms = s_dur_ms;
    }

    uint32_t data_bytes = t->file_size - t->data_off;

    uint32_t target;

    if (t->fmt == SRV_MUSIC_FMT_WAV) {
        /* WAV 是裸 PCM，按采样数精确落位 */
        int frame = t->channels * (t->bits / 8);
        if (frame <= 0) {
            return;
        }
        uint32_t byte_off = (uint32_t)((int64_t)t->sample_rate * ms / 1000) * (uint32_t)frame;
        if (byte_off > data_bytes) {
            byte_off = data_bytes;
        }
        target = t->data_off + byte_off;
    } else {
        if (s_dur_ms <= 0) {
            return;
        }
        uint32_t byte_off = (uint32_t)((int64_t)data_bytes * ms / s_dur_ms);
        if (byte_off > data_bytes) {
            byte_off = data_bytes;
        }
        target = t->data_off + byte_off;
    }

    if (t->preload != NULL) {
        /* 预读模式：直接改读取位置。没法在内存里做 MP3 帧同步，
         * 误差最多一帧（<26ms），听感上察觉不到 */
        t->preload_pos = target;
    } else {
        if (t->fmt == SRV_MUSIC_FMT_MP3) {
            target = mp3_find_sync(t->fp, target, t->data_off + data_bytes);
        }
        fseek(t->fp, (long)target, SEEK_SET);
    }

    esp_audio_simple_dec_reset(t->dec);
    t->in_len   = 0;
    t->in_used  = 0;
    t->eos      = false;
    t->pos_samples = (int64_t)t->sample_rate * ms / 1000;
    s_pos_ms = ms;
    ESP_LOGI(TAG, "跳转到 %d ms", (int)ms);
}

/* 预热：先解出格式信息（PCM 丢弃），并定出总时长 */
static bool track_prime(track_t *t)
{
    for (int guard = 0; guard < 256; guard++) {
        esp_audio_simple_dec_info_t info;
        if (esp_audio_simple_dec_get_info(t->dec, &info) == ESP_AUDIO_ERR_OK &&
            info.sample_rate > 0) {
            t->sample_rate = (int)info.sample_rate;
            t->channels    = info.channel ? info.channel : 1;
            t->bits        = info.bits_per_sample ? info.bits_per_sample : 16;

            /* 时长：MP3 有 Xing/Info 总帧数就按它算 —— VBR 逐帧码率浮动，
             * 拿解码器报的码率去除文件大小能差出好几倍（3:12 会显示成 2:20）。
             * 没有 Xing 才退回码率估算；注意 info.bitrate 的单位各版本不一致：
             * 小于 1000 的一律按 kbps 处理，否则时长会放大 1000 倍。 */
            uint32_t bitrate = info.bitrate;
            if (bitrate > 0 && bitrate < 1000) {
                bitrate *= 1000;
            }

            if (t->xing_dur_ms > 0) {
                s_dur_ms = t->xing_dur_ms;
            } else {
                uint32_t data_bytes = (t->file_size > t->data_off) ? (t->file_size - t->data_off) : 0;
                if (bitrate > 0 && data_bytes > 0) {
                    int64_t ms = (int64_t)data_bytes * 8 * 1000 / bitrate;
                    /* 超过 24 小时显然不对，当未知处理 */
                    s_dur_ms = (ms > 0 && ms < 24LL * 3600 * 1000) ? (int32_t)ms : 0;
                } else {
                    s_dur_ms = 0;
                }
            }
            ESP_LOGI(TAG, "%d Hz / %d 声道 / %d bit / %u bps / %s %d 秒",
                     t->sample_rate, t->channels, t->bits, (unsigned)bitrate,
                     (t->xing_dur_ms > 0) ? "时长" : "约", (int)(s_dur_ms / 1000));
            return true;
        }

        if (t->in_used >= t->in_len) {
            if (t->eos) {
                return false;
            }
            if (!track_fill(t)) {
                t->eos = true;
                return false;
            }
        }

        esp_audio_simple_dec_raw_t raw = {
            .buffer   = t->in_buf + t->in_used,
            .len      = (uint32_t)(t->in_len - t->in_used),
            .eos      = t->eos,
            .consumed = 0,
        };
        esp_audio_simple_dec_out_t out = {
            .buffer = t->pcm_buf,
            .len    = (uint32_t)t->pcm_cap,
        };
        esp_audio_err_t r = esp_audio_simple_dec_process(t->dec, &raw, &out);
        if (r == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            if (!pcm_grow(t, out.needed_size)) {
                return false;
            }
            continue;
        }
        if (r != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "预热解码出错 %d", (int)r);
            return false;
        }
        t->in_used += (int)raw.consumed;
    }
    return false;
}

static void track_close(track_t *t)
{
    if (t == NULL) {
        return;
    }
    bus_stat_reset();
    if (t->codec_open) {
        srv_audio_pcm_close();
    }
    if (t->dec != NULL) {
        esp_audio_simple_dec_close(t->dec);
    }
    if (t->fp != NULL) {
        fclose(t->fp);
    }
    if (t->preload != NULL) {
        free(t->preload);
    }
    free(t->in_buf);
    free(t->pcm_buf);
    free(t);
}

static track_t *track_open(const srv_music_t *m)
{
    esp_audio_simple_dec_type_t type = dec_type_of(m->fmt);
    if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
        ESP_LOGE(TAG, "不支持的格式: %s", m->path);
        return NULL;
    }

    FILE *fp = fopen(m->path, "rb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "打开失败: %s", m->path);
        return NULL;
    }

    track_t *t = calloc(1, sizeof(track_t));
    if (t == NULL) {
        fclose(fp);
        return NULL;
    }
    t->fp  = fp;
    t->fmt = m->fmt;

    fseek(fp, 0L, SEEK_END);
    long fsz = ftell(fp);
    t->file_size = (fsz > 0) ? (uint32_t)fsz : 0;

    if (m->fmt == SRV_MUSIC_FMT_WAV) {
        fseek(fp, 0L, SEEK_SET);
        t->data_off = wav_parse(fp, &t->sample_rate, &t->channels, &t->bits);
        if (t->data_off == 0) {
            ESP_LOGW(TAG, "WAV 头异常，按整文件处理: %s", m->path);
        }
    } else if (m->fmt == SRV_MUSIC_FMT_MP3) {
        /* 跳过 ID3v2 等标签直接从音频帧开始喂解码器，顺带取 Xing 里的精确时长 */
        uint32_t first = mp3_probe(fp, t->file_size, &t->xing_dur_ms);
        if (first > 0) {
            t->data_off = first;
        }
    }
    fseek(fp, (long)t->data_off, SEEK_SET);

    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type      = type,
        .dec_cfg       = NULL,
        .cfg_size      = 0,
        .use_frame_dec = false,
    };
    if (esp_audio_simple_dec_open(&cfg, &t->dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "解码器打开失败: %s", m->path);
        track_close(t);
        return NULL;
    }

    t->in_buf  = big_malloc(IN_BUF_BYTES);
    t->pcm_buf = big_malloc(PCM_BUF_BYTES);
    t->pcm_cap = PCM_BUF_BYTES;
    if (t->in_buf == NULL || t->pcm_buf == NULL) {
        ESP_LOGE(TAG, "缓冲分配失败");
        track_close(t);
        return NULL;
    }

    s_pos_ms = 0;
    s_dur_ms = 0;

    if (!track_prime(t)) {
        ESP_LOGE(TAG, "无法解析音频信息: %s", m->path);
        track_close(t);
        return NULL;
    }

    /*
     * 整首预读：SD 与 LCD 共用 SPI2，流式播放时每次 fread 都可能被刷屏阻塞，
     * 缓冲再大也只是减少次数。≤4MB 的曲子整份读进 PSRAM，播放阶段零 SD 访问，
     * 从根上消掉这类细微卡顿；更大的曲子仍按流式。
     */
    if (t->file_size > t->data_off && t->file_size <= PRELOAD_MAX) {
        uint8_t *buf = big_malloc(t->file_size);
        if (buf != NULL) {
            fseek(t->fp, 0, SEEK_SET);
            if (fread(buf, 1, t->file_size, t->fp) == t->file_size) {
                t->preload      = buf;
                t->preload_size = t->file_size;
                t->preload_pos  = t->data_off;      /* 跳过 ID3 / WAV 头 */
                fclose(t->fp);
                t->fp      = NULL;
                t->in_len  = 0;
                t->in_used = 0;
                t->eos     = false;
                ESP_LOGI(TAG, "整首预读：%u KB（播放期间不再读卡）",
                         (unsigned)(t->file_size / 1024));
            } else {
                free(buf);
                fseek(t->fp, (long)t->data_off, SEEK_SET);
            }
        } else {
            ESP_LOGW(TAG, "PSRAM 不足，%s 仍按流式播放", m->path);
        }
    }

    if (srv_audio_pcm_open(t->sample_rate, t->channels, t->bits) != ESP_OK) {
        track_close(t);
        return NULL;
    }
    t->codec_open = true;

    ESP_LOGI(TAG, "开始播放：%s", m->name);
    return t;
}

/* ===================== 选曲 ===================== */

static int32_t pick_next(bool manual)
{
    int n = queue_count();
    if (n <= 0) {
        return -1;
    }
    int32_t cur = (s_index < 0) ? 0 : s_index;
    if (cur >= n) {
        cur = 0;
    }

    if (s_mode == SRV_PLAYER_MODE_SHUFFLE) {
        if (n == 1) {
            return 0;
        }
        int32_t r = cur;
        for (int i = 0; i < 8 && r == cur; i++) {
            r = (int32_t)(esp_random() % (uint32_t)n);
        }
        return r;
    }
    if (s_mode == SRV_PLAYER_MODE_SINGLE && !manual) {
        return cur;
    }
    return (cur + 1) % n;
}

static int32_t pick_prev(void)
{
    int n = queue_count();
    if (n <= 0) {
        return -1;
    }
    int32_t cur = (s_index < 0) ? 0 : s_index;
    if (cur >= n) {
        cur = 0;
    }

    if (s_mode == SRV_PLAYER_MODE_SHUFFLE) {
        if (n == 1) {
            return 0;
        }
        int32_t r = cur;
        for (int i = 0; i < 8 && r == cur; i++) {
            r = (int32_t)(esp_random() % (uint32_t)n);
        }
        return r;
    }
    return (cur - 1 + n) % n;
}

/* ===================== 进度 ===================== */

esp_err_t srv_player_save_progress(bool force)
{
    srv_progress_music_t p;
    memset(&p, 0, sizeof(p));
    p.playlist    = s_list_id;
    p.playing     = (s_state == SRV_PLAYER_PLAYING) ? 1 : 0;
    p.index       = s_index;
    p.mode        = (int32_t)s_mode;

    const srv_music_t *m = srv_player_current();
    if (m != NULL) {
        snprintf(p.file, sizeof(p.file), "%s", m->name);
    }
    return srv_progress_music_save(&p, force);
}

static void save_progress_throttled(void)
{
    int64_t now = esp_timer_get_time();
    if ((now - s_last_save_us) < SAVE_MIN_INTERVAL_US) {
        return;
    }
    s_last_save_us = now;
    (void)srv_player_save_progress(false);
}

/* ===================== 播放任务 ===================== */

static void player_task(void *arg)
{
    (void)arg;
    track_t *t = NULL;

    for (;;) {
        player_cmd_t c;
        TickType_t wait = (t == NULL) ? portMAX_DELAY : 0;

        if (xQueueReceive(s_q, &c, wait) == pdTRUE) {
            switch (c.cmd) {
            case CMD_PLAY: {
                if (t != NULL) {
                    track_close(t);
                    t = NULL;
                }
                const srv_music_t *m = queue_item(c.arg);
                if (m == NULL) {
                    ESP_LOGW(TAG, "队列里没有第 %d 首", (int)c.arg);
                    s_state = SRV_PLAYER_IDLE;
                    break;
                }
                t = track_open(m);
                if (t == NULL) {
                    s_state = SRV_PLAYER_IDLE;
                    int32_t nx = pick_next(false);      /* 坏文件自动跳下一首 */
                    if (nx >= 0 && nx != c.arg) {
                        s_index = nx;
                        post_cmd(CMD_PLAY, nx);
                    }
                    break;
                }
                s_index = c.arg;
                s_state = SRV_PLAYER_PLAYING;
                break;
            }
            case CMD_PAUSE:
                if (t != NULL) {
                    s_state = SRV_PLAYER_PAUSED;
                    (void)srv_player_save_progress(true);
                }
                break;
            case CMD_RESUME:
                if (t != NULL) {
                    s_state = SRV_PLAYER_PLAYING;
                }
                break;
            case CMD_STOP:
                if (t != NULL) {
                    track_close(t);
                    t = NULL;
                }
                s_state = SRV_PLAYER_IDLE;
                s_pos_ms = 0;
                (void)srv_player_save_progress(true);
                break;
            case CMD_NEXT:
            case CMD_PREV: {
                if (t != NULL) {
                    track_close(t);
                    t = NULL;
                }
                int32_t nx = (c.cmd == CMD_NEXT) ? pick_next(true) : pick_prev();
                if (nx < 0) {
                    s_state = SRV_PLAYER_IDLE;
                    break;
                }
                s_index = nx;
                post_cmd(CMD_PLAY, nx);
                break;
            }
            case CMD_SEEK:
                if (t != NULL) {
                    track_seek(t, c.arg);
                }
                break;
            default:
                break;
            }
        }

        if (t == NULL) {
            continue;
        }

        if (s_state == SRV_PLAYER_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        int r = track_step(t);
        if (r > 0) {
            save_progress_throttled();
            continue;
        }

        track_close(t);
        t = NULL;

        if (r < 0) {
            s_state = SRV_PLAYER_IDLE;
            ESP_LOGW(TAG, "播放出错，已停止");
            continue;
        }

        /* 本曲播完 → 按播放方式选下一首 */
        int32_t nx = pick_next(false);
        if (nx < 0) {
            s_state = SRV_PLAYER_IDLE;
            continue;
        }
        s_index = nx;
        post_cmd(CMD_PLAY, nx);
    }
}

/* ===================== 对外接口 ===================== */

esp_err_t srv_player_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "解码器注册失败");
        return ESP_FAIL;
    }

    s_q = xQueueCreate(CMD_Q_LEN, sizeof(player_cmd_t));
    if (s_q == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 播放任务栈放 PSRAM，省内部 RAM */
    BaseType_t ok = xTaskCreateWithCaps(player_task, "music_play",
                                        PLAYER_TASK_STACK, NULL, PLAYER_TASK_PRIO,
                                        &s_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ok = xTaskCreate(player_task, "music_play", PLAYER_TASK_STACK, NULL,
                         PLAYER_TASK_PRIO, &s_task);
    }
    if (ok != pdPASS) {
        vQueueDelete(s_q);
        s_q = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "播放器就绪");
    return ESP_OK;
}

esp_err_t srv_player_play_playlist(int list_id, int index)
{
    if (srv_musiclib_playlist_open(list_id) != ESP_OK) {
        return ESP_FAIL;
    }
    s_list_id = list_id;

    int n = srv_musiclib_playlist_item_count();
    if (index < 0 || index >= n) {
        ESP_LOGW(TAG, "播放列表 %d 里没有第 %d 首（共 %d 首）", list_id, index, n);
        return ESP_ERR_INVALID_ARG;
    }

    post_cmd(CMD_PLAY, index);
    return ESP_OK;
}

esp_err_t srv_player_set_playlist(int list_id)
{
    if (list_id < 1 || srv_musiclib_playlist_open(list_id) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    s_list_id = list_id;

    /* 全部循环时队列就是媒体库，跟当前播放列表无关：切列表只是换下次要放的
     * 那份名单，正在放的歌不该被打断（否则听歌时翻一下列表就断掉） */
    if (queue_is_library()) {
        ESP_LOGI(TAG, "切换到播放列表 %d（全部循环中，播放不中断）", list_id);
        return ESP_OK;
    }

    /* 列表换了，当前曲目序号可能越界 */
    int n = srv_musiclib_playlist_item_count();
    if (s_index >= n) {
        s_index = (n > 0) ? 0 : -1;
    }
    if (s_state != SRV_PLAYER_IDLE) {
        post_cmd(CMD_STOP, 0);
    }
    ESP_LOGI(TAG, "切换到播放列表 %d（%d 首）", list_id, n);
    return ESP_OK;
}

esp_err_t srv_player_play_index(int index)
{
    if (queue_item(index) == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    post_cmd(CMD_PLAY, index);
    return ESP_OK;
}

esp_err_t srv_player_toggle(void)
{
    if (s_state == SRV_PLAYER_PLAYING) {
        return srv_player_pause();
    }
    if (s_state == SRV_PLAYER_PAUSED) {
        return srv_player_resume();
    }
    if (s_index < 0) {
        s_index = 0;
    }
    post_cmd(CMD_PLAY, s_index);
    return ESP_OK;
}

esp_err_t srv_player_pause(void)
{
    post_cmd(CMD_PAUSE, 0);
    return ESP_OK;
}

esp_err_t srv_player_resume(void)
{
    post_cmd(CMD_RESUME, 0);
    return ESP_OK;
}

esp_err_t srv_player_stop(void)
{
    post_cmd(CMD_STOP, 0);
    return ESP_OK;
}

esp_err_t srv_player_next(void)
{
    post_cmd(CMD_NEXT, 0);
    return ESP_OK;
}

esp_err_t srv_player_prev(void)
{
    post_cmd(CMD_PREV, 0);
    return ESP_OK;
}

esp_err_t srv_player_seek_ms(int ms)
{
    if (ms < 0) {
        ms = 0;
    }
    post_cmd(CMD_SEEK, ms);
    return ESP_OK;
}

/* ===================== 状态查询 ===================== */

srv_player_state_t srv_player_state(void)
{
    return s_state;
}

const srv_music_t *srv_player_current(void)
{
    return queue_item(s_index);
}

int srv_player_index(void)
{
    return s_index;
}

int srv_player_queue_count(void)
{
    return queue_count();
}

esp_err_t srv_player_queue_item(int idx, const srv_music_t **out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const srv_music_t *m = queue_item(idx);
    if (m == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = m;
    return ESP_OK;
}

int srv_player_playlist_id(void)
{
    return s_list_id;
}

int srv_player_position_ms(void)
{
    return s_pos_ms;
}

int srv_player_duration_ms(void)
{
    return s_dur_ms;
}

/* ===================== 播放方式 ===================== */

void srv_player_set_mode(srv_player_mode_t mode)
{
    if (mode < 0 || mode >= SRV_PLAYER_MODE_NUM || mode == s_mode) {
        return;
    }

    /* 队列来源变了，按当前曲目路径在新队列里重新定位 */
    char path[SRV_FS_PATH_MAX];
    path[0] = '\0';
    const srv_music_t *cur = srv_player_current();
    if (cur != NULL) {
        snprintf(path, sizeof(path), "%s", cur->path);
    }

    s_mode = mode;

    if (path[0] != '\0') {
        int n = queue_count();
        for (int i = 0; i < n; i++) {
            const srv_music_t *m = queue_item(i);
            if (m != NULL && strcmp(m->path, path) == 0) {
                s_index = i;
                break;
            }
        }
    }
    ESP_LOGI(TAG, "播放方式 -> %d", (int)mode);
}

srv_player_mode_t srv_player_mode(void)
{
    return s_mode;
}

/* ===================== 进度恢复 ===================== */

esp_err_t srv_player_restore(void)
{
    srv_progress_music_t p;
    if (srv_progress_music_load(&p) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    if (p.playlist < 1 || srv_musiclib_playlist_open(p.playlist) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    s_list_id = p.playlist;
    if (p.mode >= 0 && p.mode < SRV_PLAYER_MODE_NUM) {
        s_mode = (srv_player_mode_t)p.mode;
    }

    int n = srv_musiclib_playlist_item_count();
    if (n <= 0) {
        return ESP_ERR_NOT_FOUND;
    }

    int idx = (p.index >= 0 && p.index < n) ? p.index : 0;

    /* 列表内容可能变了：文件名对不上就按名字重新找 */
    const srv_music_t *m = NULL;
    (void)srv_musiclib_playlist_item(idx, &m);
    if (m != NULL && p.file[0] != '\0' && strcmp(m->name, p.file) != 0) {
        for (int i = 0; i < n; i++) {
            const srv_music_t *it = NULL;
            if (srv_musiclib_playlist_item(i, &it) == ESP_OK && it != NULL &&
                strcmp(it->name, p.file) == 0) {
                idx = i;
                break;
            }
        }
    }

    s_index   = idx;
    s_pos_ms  = 0;
    s_dur_ms  = 0;
    s_state   = SRV_PLAYER_IDLE;

    ESP_LOGI(TAG, "恢复上次播放：列表 %d 第 %d 首", (int)p.playlist, idx);
    return ESP_OK;
}
