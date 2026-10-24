#include "srv_radio.h"

#include "srv_audio.h"
#include "srv_player.h"
#include "srv_wifi.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "srv_radio";

/* ===================== 常量 ===================== */

/*! 中国之声（央广）直播源。纯 http，不需要证书包 */
#define RADIO_M3U8_URL   "http://ngcdn001.cnr.cn/live/zgzs/index.m3u8"

#define HTTP_TIMEOUT_MS  8000
#define HTTP_UA          "clock-reader/1.0 (ESP32-C5)"
#define PL_BUF_BYTES     4096       /*!< 播放列表最大长度（实际 ~600 字节） */
#define SEG_NAME_MAX     96
#define SEG_MAX          16         /*!< 列表里最多记这么多分片名（实际 6 片） */
#define SEG_URL_MAX      240
#define IN_BUF_BYTES     8192       /*!< 压缩数据缓冲：一次 HTTP 读这么多 */
#define PCM_BUF_BYTES    8192
#define PCM_BUF_MAX      (64 * 1024)
#define CMD_Q_LEN        8
#define TASK_STACK       8192       /*!< 比播放器宽一点：这层还要跑 HTTP 客户端 */
#define TASK_PRIO        5
#define RETRY_MS_MAX     8000
#define START_SKIP       2          /*!< 起播时跳过窗口最前面的几片，留点缓冲垫 */

/*! 状态行缓冲（"正在播放 · 96 kbps" 这种） */
#define STATUS_MAX       48

enum {
    CMD_PLAY = 1,
    CMD_PAUSE,
    CMD_RESUME,
    CMD_STOP,
};

typedef struct {
    int32_t cmd;
} radio_cmd_t;

/* ===================== 播放上下文（只在电台任务里用） ===================== */

typedef struct {
    esp_audio_simple_dec_handle_t dec;
    uint8_t *in_buf;
    int      in_len;
    int      in_used;
    uint8_t *pcm_buf;
    int      pcm_cap;
    bool     codec_open;
    bool     codec_warned;

    esp_http_client_handle_t seg;   /*!< 当前分片的连接（NULL = 该开下一片了） */

    uint32_t first_seq;             /*!< 播放列表里第一片的序号 */
    int      seg_count;             /*!< 本次列表里记下的分片数 */
    char     names[SEG_MAX][SEG_NAME_MAX];
    char     base[128];             /*!< 分片 URL 前缀（播放列表所在目录） */

    uint32_t next_seq;              /*!< 下一个要拉的分片序号 */
    bool     need_playlist;         /*!< 需要（重新）拉播放列表 */
    int      fail_count;            /*!< 连续失败次数（算退避用） */
} radio_ctx_t;

static radio_ctx_t s_ctx;

static volatile srv_radio_state_t s_state = SRV_RADIO_IDLE;
static char s_status[STATUS_MAX] = "已停止";
static int  s_kbps = 0;

static TaskHandle_t  s_task = NULL;
static QueueHandle_t s_q    = NULL;

/*! 退避到什么时候才允许再试（0 = 不在退避中） */
static int64_t s_retry_at_us = 0;

/* ===================== 小工具 ===================== */

static void *big_malloc(size_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = malloc(bytes);
    }
    return p;
}

static void set_status(srv_radio_state_t st, const char *text)
{
    s_state = st;
    if (text != NULL) {
        snprintf(s_status, sizeof(s_status), "%s", text);
    }
}

/* 失败退避：1s / 2s / 4s / 8s，之后一直 8s；没网时固定 3s 试一次 */
static void schedule_retry(radio_ctx_t *c)
{
    if (c->fail_count < 4) {
        c->fail_count++;
    }

    int delay_ms = 1000 << (c->fail_count - 1);
    if (delay_ms > RETRY_MS_MAX) {
        delay_ms = RETRY_MS_MAX;
    }

    if (!srv_wifi_is_connected()) {
        delay_ms = 3000;
        set_status(SRV_RADIO_BUFFERING, "网络未连接");
    } else {
        set_status(SRV_RADIO_BUFFERING, "连接失败，重试中…");
    }

    s_retry_at_us = esp_timer_get_time() + (int64_t)delay_ms * 1000;
    ESP_LOGW(TAG, "%d ms 后重试（连续第 %d 次失败）", delay_ms, c->fail_count);
}

static void close_seg(radio_ctx_t *c)
{
    if (c->seg == NULL) {
        return;
    }
    esp_http_client_close(c->seg);
    esp_http_client_cleanup(c->seg);
    c->seg = NULL;
}

/* ===================== 播放列表 ===================== */

/* 分片 URL = 播放列表目录 + 分片名（名字是相对路径；也兼容写绝对 URL 的源） */
static void build_seg_url(const radio_ctx_t *c, int idx, char *out, size_t out_len)
{
    const char *nm = c->names[idx];
    if (strncmp(nm, "http://", 7) == 0 || strncmp(nm, "https://", 8) == 0) {
        snprintf(out, out_len, "%s", nm);
    } else {
        snprintf(out, out_len, "%s%s", c->base, nm);
    }
}

/**
 * @brief 解析 m3u8：取 MEDIA-SEQUENCE 和各分片名
 *
 * 直播列表里没有 ENDLIST，序列号随窗口滑动一直涨，所以只认“序号 + 文件名”，
 * 不缓存整个列表。
 */
static bool parse_playlist(const char *text, radio_ctx_t *c)
{
    uint32_t seq = 0;
    bool     has_seq = false;
    int      count = 0;

    const char *p = text;
    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t len = (eol != NULL) ? (size_t)(eol - p) : strlen(p);

        while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == ' ')) {
            len--;
        }

        if (len > 0 && p[0] != '#') {
            if (count < SEG_MAX) {
                size_t n = (len < (size_t)(SEG_NAME_MAX - 1)) ? len : (size_t)(SEG_NAME_MAX - 1);
                memcpy(c->names[count], p, n);
                c->names[count][n] = '\0';
            }
            count++;
        } else if (len >= 22 && strncmp(p, "#EXT-X-MEDIA-SEQUENCE:", 22) == 0) {
            seq = (uint32_t)strtoul(p + 22, NULL, 10);
            has_seq = true;
        }

        if (eol == NULL) {
            break;
        }
        p = eol + 1;
    }

    if (!has_seq || count == 0) {
        ESP_LOGW(TAG, "播放列表里没有 MEDIA-SEQUENCE 或分片");
        return false;
    }

    c->first_seq = seq;
    c->seg_count = (count > SEG_MAX) ? SEG_MAX : count;

    if (c->next_seq == 0) {
        /* 起播：从窗口里偏后的位置开始，前面几片当缓冲垫，
         * 免得一去就顶在“下一片还没发布”上 */
        int skip = START_SKIP;
        if (skip > c->seg_count - 1) {
            skip = c->seg_count - 1;
        }
        c->next_seq = c->first_seq + (uint32_t)skip;
    } else if (c->next_seq < c->first_seq) {
        /* 播放追不上窗口滑动（长时间卡顿/断网）：从窗口开头接上 */
        ESP_LOGW(TAG, "落后出直播窗口，跳到窗口开头 %u", (unsigned)c->first_seq);
        c->next_seq = c->first_seq;
    }

    return true;
}

/**
 * @brief 拉一次播放列表（GET index.m3u8）
 * @return true = 解析成功
 */
static bool fetch_playlist(radio_ctx_t *c)
{
    if (!srv_wifi_is_connected()) {
        schedule_retry(c);
        return false;
    }

    char *buf = big_malloc(PL_BUF_BYTES);
    if (buf == NULL) {
        schedule_retry(c);
        return false;
    }

    int n = -1;
    esp_http_client_config_t cfg = {
        .url         = RADIO_M3U8_URL,
        .user_agent  = HTTP_UA,
        .timeout_ms  = HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli != NULL) {
        if (esp_http_client_open(cli, 0) == ESP_OK) {
            (void)esp_http_client_fetch_headers(cli);
            if (esp_http_client_get_status_code(cli) == 200) {
                n = 0;
                while (n < PL_BUF_BYTES - 1) {
                    int r = esp_http_client_read(cli, buf + n, PL_BUF_BYTES - 1 - n);
                    if (r <= 0) {
                        break;
                    }
                    n += r;
                }
            } else {
                ESP_LOGW(TAG, "播放列表 HTTP %d", esp_http_client_get_status_code(cli));
            }
        }
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
    }
    if (n > 0) {
        buf[n] = '\0';
    }

    bool ok = (n > 0) && parse_playlist(buf, c);
    free(buf);

    if (!ok) {
        schedule_retry(c);
        return false;
    }

    /* 分片 URL 前缀 = 播放列表 URL 去掉最后一段 */
    snprintf(c->base, sizeof(c->base), "%s", RADIO_M3U8_URL);
    char *slash = strrchr(c->base, '/');
    if (slash != NULL) {
        slash[1] = '\0';
    }

    c->need_playlist = false;
    c->fail_count    = 0;
    s_retry_at_us    = 0;

    ESP_LOGI(TAG, "播放列表：首片 %u，共 %d 片，下一片拉 %u",
             (unsigned)c->first_seq, c->seg_count, (unsigned)c->next_seq);
    return true;
}

/**
 * @brief 发起一次流式 GET（只建立连接，数据交给 esp_http_client_read 慢慢读）
 */
static esp_http_client_handle_t http_open_stream(const char *url)
{
    esp_http_client_config_t cfg = {
        .url         = url,
        .user_agent  = HTTP_UA,
        .timeout_ms  = HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) {
        return NULL;
    }
    if (esp_http_client_open(cli, 0) != ESP_OK) {
        ESP_LOGW(TAG, "分片连接失败: %s", url);
        esp_http_client_cleanup(cli);
        return NULL;
    }
    (void)esp_http_client_fetch_headers(cli);
    if (esp_http_client_get_status_code(cli) != 200) {
        ESP_LOGW(TAG, "分片 HTTP %d", esp_http_client_get_status_code(cli));
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        return NULL;
    }
    return cli;
}

/**
 * @brief 填一块压缩数据进 in_buf（不够就顺手把下一片也拉起来）
 * @return true = 有数据可解；false = 本轮没拿到（退避已安排好）
 */
static bool fill_input(radio_ctx_t *c)
{
    for (;;) {
        /* 当前分片还有得读就接着读 */
        if (c->seg != NULL) {
            int n = esp_http_client_read(c->seg, (char *)c->in_buf, IN_BUF_BYTES);
            if (n > 0) {
                c->in_len  = n;
                c->in_used = 0;
                return true;
            }
            /* 读完（0）或断流（<0）：这一片到此为止 */
            if (n < 0) {
                ESP_LOGW(TAG, "分片读取出错，跳到下一片");
            }
            close_seg(c);
            continue;
        }

        if (c->need_playlist) {
            if (!fetch_playlist(c)) {
                return false;
            }
        }

        /* 落后到窗口外面了：从窗口开头接上（宁可跳一段，也不要卡死） */
        if (c->next_seq < c->first_seq) {
            c->next_seq = c->first_seq;
        }

        int idx = (int)(c->next_seq - c->first_seq);
        if (idx >= c->seg_count) {
            /* 已经顶到直播前沿：新片每 ~10 秒才出一片，等一会儿重拉列表 */
            ESP_LOGI(TAG, "已到直播前沿，等新分片…");
            c->need_playlist = true;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        char url[SEG_URL_MAX];
        build_seg_url(c, idx, url, sizeof(url));

        c->seg = http_open_stream(url);
        if (c->seg == NULL) {
            schedule_retry(c);
            return false;
        }

        ESP_LOGI(TAG, "拉取分片 %u（列表第 %d/%d 片）",
                 (unsigned)c->next_seq, idx + 1, c->seg_count);
        c->next_seq++;
        c->fail_count = 0;
        s_retry_at_us = 0;
        /* 回到循环开头读第一块 */
    }
}

/* ===================== 解码与播放 ===================== */

static bool pcm_grow(radio_ctx_t *c, uint32_t need)
{
    if ((int)need > PCM_BUF_MAX) {
        return false;
    }
    if ((int)need <= c->pcm_cap) {
        return true;
    }
    uint8_t *nb = big_malloc(need);
    if (nb == NULL) {
        return false;
    }
    free(c->pcm_buf);
    c->pcm_buf = nb;
    c->pcm_cap = (int)need;
    return true;
}

/* 首帧解出来后按解码器报的真实格式打开扬声器通道 */
static void open_codec(radio_ctx_t *c)
{
    esp_audio_simple_dec_info_t info;
    if (esp_audio_simple_dec_get_info(c->dec, &info) != ESP_AUDIO_ERR_OK ||
        info.sample_rate == 0) {
        return;                     /* 格式还没出来，等下一帧 */
    }

    int      rate     = (int)info.sample_rate;
    int      channels = info.channel ? (int)info.channel : 2;
    int      bits     = info.bits_per_sample ? (int)info.bits_per_sample : 16;
    uint32_t br       = info.bitrate;
    if (br > 0 && br < 1000) {
        br *= 1000;                 /* 有的解码器报的是 kbps */
    }

    if (srv_audio_pcm_open(rate, channels, bits) != ESP_OK) {
        if (!c->codec_warned) {
            c->codec_warned = true;
            ESP_LOGE(TAG, "扬声器通道打开失败");
        }
        return;
    }

    c->codec_open = true;
    s_kbps = (int)(br / 1000);
    ESP_LOGI(TAG, "%d Hz / %d 声道 / %d bit / %d kbps", rate, channels, bits, s_kbps);
}

/**
 * @brief 解一帧并写进扬声器
 * @return true = 有进展（解出了数据）
 */
static bool decode_step(radio_ctx_t *c)
{
    for (int guard = 0; guard < 8; guard++) {
        esp_audio_simple_dec_raw_t raw = {
            .buffer = c->in_buf + c->in_used,
            .len    = (uint32_t)(c->in_len - c->in_used),
            .eos    = false,
            .consumed = 0,
        };
        esp_audio_simple_dec_out_t out = {
            .buffer = c->pcm_buf,
            .len    = (uint32_t)c->pcm_cap,
        };

        esp_audio_err_t r = esp_audio_simple_dec_process(c->dec, &raw, &out);

        if (r == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            if (!pcm_grow(c, out.needed_size)) {
                ESP_LOGE(TAG, "PCM 缓冲不够（需要 %u 字节）", (unsigned)out.needed_size);
                return false;
            }
            continue;
        }

        c->in_used += (int)raw.consumed;

        if (r != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "解码出错 %d，丢掉当前数据块", (int)r);
            c->in_used = c->in_len;
            return false;
        }

        if (out.decoded_size == 0) {
            if (raw.consumed == 0) {
                return false;           /* 攒不满一帧，回去再读数据 */
            }
            continue;
        }

        if (!c->codec_open) {
            open_codec(c);
            if (!c->codec_open) {
                return true;            /* 格式信息还没出来，这帧先丢掉 */
            }
        }

        srv_audio_pcm_write(c->pcm_buf, (int)out.decoded_size);

        if (s_state != SRV_RADIO_PLAYING) {
            if (s_kbps > 0) {
                char buf[STATUS_MAX];
                snprintf(buf, sizeof(buf), "正在播放 · %d kbps", s_kbps);
                set_status(SRV_RADIO_PLAYING, buf);
            } else {
                set_status(SRV_RADIO_PLAYING, "正在播放");
            }
        }
        return true;
    }
    return false;
}

/**
 * @brief 走一小步：没数据就拉数据，有数据就解一帧播出去
 * @return true = 有进展
 */
static bool radio_step(radio_ctx_t *c)
{
    if (s_retry_at_us != 0) {
        if (esp_timer_get_time() < s_retry_at_us) {
            return false;               /* 还在退避里 */
        }
        s_retry_at_us = 0;
    }

    if (c->in_used >= c->in_len) {
        if (!fill_input(c)) {
            return false;
        }
    }
    return decode_step(c);
}

/* ===================== 起播 / 收尾 ===================== */

/* 关闭一切、清空位置（可重入：stop 后再 start 是干净的） */
static void ctx_reset(radio_ctx_t *c)
{
    close_seg(c);

    if (c->dec != NULL) {
        esp_audio_simple_dec_close(c->dec);
        c->dec = NULL;
    }

    free(c->in_buf);
    free(c->pcm_buf);
    c->in_buf  = NULL;
    c->pcm_buf = NULL;
    c->pcm_cap = 0;
    c->in_len  = 0;
    c->in_used = 0;

    if (c->codec_open) {
        srv_audio_pcm_close();
        c->codec_open = false;
    }

    c->next_seq      = 0;
    c->first_seq     = 0;
    c->seg_count     = 0;
    c->need_playlist = true;
    c->fail_count    = 0;
    c->codec_warned  = false;

    s_retry_at_us = 0;
    s_kbps        = 0;
}

/**
 * @brief 起播/重连：开解码器和缓冲，等第一次拉列表时对齐直播窗口
 */
static void ctx_start(radio_ctx_t *c)
{
    /* TS 是**容器**解码器，要单独注册（音乐那边只注册了 MP3/AAC 这些裸解码器，
     * 见 srv_player_init 的 esp_audio_dec_register_default）。只做一次。 */
    static bool s_ts_registered = false;
    if (!s_ts_registered) {
        s_ts_registered = true;
        if (esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "TS 容器解码器注册失败，可能解不出声音");
        }
    }

    ctx_reset(c);

    c->in_buf  = big_malloc(IN_BUF_BYTES);
    c->pcm_buf = big_malloc(PCM_BUF_BYTES);
    c->pcm_cap = (c->pcm_buf != NULL) ? PCM_BUF_BYTES : 0;

    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type      = ESP_AUDIO_SIMPLE_DEC_TYPE_TS,
        .dec_cfg       = NULL,
        .cfg_size      = 0,
        .use_frame_dec = false,
    };
    if (c->in_buf == NULL || c->pcm_buf == NULL ||
        esp_audio_simple_dec_open(&cfg, &c->dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "电台初始化失败");
        ctx_reset(c);
        set_status(SRV_RADIO_IDLE, "启动失败");
        return;
    }

    set_status(SRV_RADIO_BUFFERING, "正在连接…");
    ESP_LOGI(TAG, "开始收听 %s", SRV_RADIO_NAME);
}

/* ===================== 电台任务 ===================== */

static void handle_cmd(radio_ctx_t *c, int32_t cmd)
{
    switch (cmd) {
    case CMD_PLAY:
        if (s_state != SRV_RADIO_BUFFERING && s_state != SRV_RADIO_PLAYING) {
            ctx_start(c);
        }
        break;

    case CMD_PAUSE:
        if (s_state == SRV_RADIO_BUFFERING || s_state == SRV_RADIO_PLAYING) {
            ctx_reset(c);
            set_status(SRV_RADIO_PAUSED, "已暂停");
            ESP_LOGI(TAG, "暂停");
        }
        break;

    case CMD_RESUME:
        if (s_state == SRV_RADIO_PAUSED) {
            ctx_start(c);
        }
        break;

    case CMD_STOP:
        if (s_state != SRV_RADIO_IDLE) {
            ctx_reset(c);
            set_status(SRV_RADIO_IDLE, "已停止");
            ESP_LOGI(TAG, "停止");
        }
        break;

    default:
        break;
    }
}

static void radio_task(void *arg)
{
    (void)arg;
    radio_ctx_t *c = &s_ctx;

    for (;;) {
        bool busy = (s_state == SRV_RADIO_BUFFERING || s_state == SRV_RADIO_PLAYING);

        radio_cmd_t cmd;
        if (xQueueReceive(s_q, &cmd, busy ? 0 : pdMS_TO_TICKS(50)) == pdTRUE) {
            handle_cmd(c, cmd.cmd);
        }

        /* 播放/缓冲中：不断往解码器喂数据、往扬声器写 PCM */
        if (s_state == SRV_RADIO_BUFFERING || s_state == SRV_RADIO_PLAYING) {
            if (!radio_step(c)) {
                vTaskDelay(pdMS_TO_TICKS(20));   /* 退避 / 等新片时别空转 */
            }
        }
        /* 空闲/暂停：上面那句 xQueueReceive 的 50ms 就是节拍，不用再睡 */
    }
}

static bool ensure_task(void)
{
    if (s_task != NULL) {
        return true;
    }

    s_q = xQueueCreate(CMD_Q_LEN, sizeof(radio_cmd_t));
    if (s_q == NULL) {
        return false;
    }

    /* 电台任务栈放 PSRAM，省内部 RAM（和音乐播放任务一致的做法） */
    BaseType_t ok = xTaskCreateWithCaps(radio_task, "radio", TASK_STACK, NULL, TASK_PRIO,
                                        &s_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ok = xTaskCreate(radio_task, "radio", TASK_STACK, NULL, TASK_PRIO, &s_task);
    }
    if (ok != pdPASS) {
        vQueueDelete(s_q);
        s_q = NULL;
        return false;
    }
    return true;
}

static esp_err_t post_cmd(int32_t cmd)
{
    if (!ensure_task()) {
        ESP_LOGE(TAG, "电台任务创建失败");
        return ESP_FAIL;
    }
    radio_cmd_t c = { .cmd = cmd };
    if (xQueueSend(s_q, &c, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ===================== 对外接口 ===================== */

esp_err_t srv_radio_start(void)
{
    /* 音乐占着扬声器通道时先停掉（两边共用 ES8311） */
    if (srv_player_state() != SRV_PLAYER_IDLE) {
        (void)srv_player_stop();
        ESP_LOGI(TAG, "停掉音乐，把扬声器让给电台");
    }
    return post_cmd(CMD_PLAY);
}

esp_err_t srv_radio_pause(void)
{
    return post_cmd(CMD_PAUSE);
}

esp_err_t srv_radio_resume(void)
{
    return post_cmd(CMD_RESUME);
}

esp_err_t srv_radio_stop(void)
{
    if (s_task == NULL) {
        return ESP_OK;              /* 从没起播过，没什么可停的 */
    }
    return post_cmd(CMD_STOP);
}

srv_radio_state_t srv_radio_state(void)
{
    return s_state;
}

const char *srv_radio_status_text(void)
{
    return s_status;
}