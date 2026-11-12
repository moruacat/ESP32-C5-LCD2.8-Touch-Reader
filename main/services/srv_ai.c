#include "srv_ai.h"
#include "srv_ai_tools.h"

#include "srv_audio.h"
#include "srv_ntp.h"
#include "srv_player.h"
#include "srv_radio.h"
#include "srv_wifi.h"

#include "esp_xiaozhi_chat.h"
#include "esp_xiaozhi_info.h"
#include "esp_mcp_engine.h"
#include "esp_opus_dec.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "srv_ai";

/* 音频格式是"混合"的，实测服务端就是这么工作的：
 *   - 下行 TTS 固定回 Opus 码流（hello 里写 pcm 也一样），必须自己解码
 *   - 上行按 hello 的 format 解析，用 PCM 可以省掉 Opus 编码器
 *     （编码器要 32KB 栈，而内部 RAM 只剩几十 KB，放不下）
 *
 * 上行 PCM 一帧 40ms（16k 单声道 16bit → 1280 字节）：服务端优先走
 * MQTT + UDP，UDP 单包明文上限 CONFIG_XIAOZHI_UDP_MAX_AUDIO_PAYLOAD_SIZE=1400，
 * 60ms 的 1920 字节会被整帧丢弃。 */
#define AI_SAMPLE_RATE   16000      /*!< 上行采样率（麦克风） */
#define AI_FRAME_MS      40
#define AI_TX_BYTES      (AI_SAMPLE_RATE * AI_FRAME_MS / 1000 * 2)   /*!< 1280 字节 PCM */

/*! TTS 下行统一按 24k 解码/播放：小智的语音合成默认就是这个采样率，
 * 就算服务端按 16k 编码，Opus 解码器也会自动重采样到 24k 输出 */
#define AI_TTS_RATE      24000
#define AI_DEC_PCM_MAX   (AI_TTS_RATE * 120 / 1000 * 2)              /*!< 5760 字节，够 120ms */

/*! 会话任务不做编解码（编码器已去掉，解码在组件音频任务里），
 *  只需要普通调用栈；之前 9KB 跑 get_info 是够的 */
#define AI_TASK_STACK    10240
#define AI_TASK_PRIO     5

/*! 服务端约 60~70 秒收不到音频就会 goodbye 结束会话（日志实测）。
 *  空闲时每 25 秒补一帧 40ms 静音，会话能一直挂着，对话上下文也就不会断。 */
#define AI_KEEPALIVE_US  (25 * 1000 * 1000LL)

static volatile bool            s_task_alive = false;
static volatile bool            s_stop_req = false;
static volatile bool            s_talk_req = false;
static volatile bool            s_abort_req = false;   /*!< 用户要在 AI 说话时打断 */
static volatile bool            s_link_lost = false;   /*!< MQTT 断开 / 服务端结束会话 */
static volatile int64_t         s_reply_deadline = 0;  /*!< 说完话后等回应的截止时刻（us） */
static volatile srv_ai_state_t  s_state = SRV_AI_IDLE;

static esp_xiaozhi_chat_handle_t s_chat = 0;
static esp_xiaozhi_chat_info_t   s_info;        /*!< get_info 的结果，任务内使用 */
static char                      s_act_code[16];
static char                      s_last_text[160];
static char                      s_status[64];
static bool                      s_spk_open = false;

/* Opus 解码器与缓冲（只用于下行 TTS） */
static void    *s_dec = NULL;
static uint8_t *s_pcm_in = NULL;    /*!< 麦克风 PCM（AI_TX_BYTES） */
static uint8_t *s_dec_out = NULL;   /*!< 下行解码 PCM（AI_DEC_PCM_MAX） */

/* ===================== 状态文本 ===================== */

/* 音频缓冲优先放 PSRAM：内部 RAM 要留给 TLS/WiFi */
static void *big_malloc(size_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = malloc(bytes);
    }
    return p;
}

static void set_state(srv_ai_state_t st, const char *text)
{
    s_state = st;
    snprintf(s_status, sizeof(s_status), "%s", (text != NULL) ? text : "");
    ESP_LOGI(TAG, "状态 -> %d（%s）", (int)st, s_status);
}

srv_ai_state_t srv_ai_state(void)
{
    return s_state;
}

const char *srv_ai_state_text(void)
{
    return s_status;
}

const char *srv_ai_activation_code(void)
{
    return s_act_code;
}

const char *srv_ai_last_text(void)
{
    return s_last_text;
}

bool srv_ai_running(void)
{
    return s_task_alive;
}

/* ===================== 音频 ===================== */

/**
 * @brief 下行 TTS 音频（组件任务回调）
 *
 * 回调里必须当场消费掉数据，指针出了回调就失效。数据是 Opus 裸帧，
 * 先解码成 24k PCM 再写 I2S；写 I2S 是阻塞的，正好起到限速作用。
 */
static void ai_audio_cb(const uint8_t *data, int len, void *ctx)
{
    (void)ctx;
    static int log_left = 4;        /* 前几帧打一下长度，排查格式问题 */
    if (data == NULL || len <= 0) {
        return;
    }
    /* 正在听、或用户刚打断：丢弃还在路上的 TTS 帧，
     * 否则会把喇叭重新打开，AI 的声音又被麦克风录进去 */
    if (s_talk_req || s_state == SRV_AI_LISTENING) {
        return;
    }
    if (log_left > 0) {
        ESP_LOGI(TAG, "TTS 下行帧 %d 字节", len);
        log_left--;
    }

    if (s_dec == NULL || s_dec_out == NULL) {
        return;
    }

    esp_audio_dec_in_raw_t raw = {
        .buffer = (uint8_t *)data,
        .len    = (uint32_t)len,
    };
    esp_audio_dec_out_frame_t frame = {
        .buffer = s_dec_out,
        .len    = AI_DEC_PCM_MAX,
    };
    esp_audio_dec_info_t info;

    esp_audio_err_t r = esp_opus_dec_decode(s_dec, &raw, &frame, &info);
    if (r != ESP_AUDIO_ERR_OK || frame.decoded_size == 0) {
        return;
    }

    if (!s_spk_open) {
        if (srv_audio_pcm_open(AI_TTS_RATE, 1, 16) == ESP_OK) {
            s_spk_open = true;
        } else {
            return;                 /* 打不开就别反复尝试，下一帧再说 */
        }
    }
    (void)srv_audio_pcm_write(s_dec_out, (int)frame.decoded_size);
}

static void ai_event_cb(esp_xiaozhi_chat_event_t event, void *event_data, void *ctx)
{
    (void)ctx;

    switch (event) {
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE: {
        const esp_xiaozhi_chat_tts_state_t *ts = (const esp_xiaozhi_chat_tts_state_t *)event_data;
        if (ts == NULL) {
            break;
        }
        if (ts->state == ESP_XIAOZHI_CHAT_TTS_STATE_START) {
            s_reply_deadline = 0;       /* 有回应了，撤销"哑会话"计时 */
            set_state(SRV_AI_SPEAKING, "正在回答…");
        } else if (ts->state == ESP_XIAOZHI_CHAT_TTS_STATE_STOP) {
            if (s_state == SRV_AI_SPEAKING) {
                set_state(SRV_AI_READY, "可以说话");
            }
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SPEECH_STARTED:
        s_reply_deadline = 0;
        set_state(SRV_AI_SPEAKING, "正在回答…");
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SPEECH_STOPPED:
        if (s_state == SRV_AI_SPEAKING) {
            set_state(SRV_AI_READY, "可以说话");
        }
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_ERROR: {
        const esp_xiaozhi_chat_error_info_t *err = (const esp_xiaozhi_chat_error_info_t *)event_data;
        ESP_LOGE(TAG, "协议错误 %d（%s）", err ? (int)err->code : -1,
                 (err && err->source) ? err->source : "?");
        set_state(SRV_AI_ERROR, "连接出错");
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT: {
        /* STT/TTS 文本：排查「服务端到底有没有收到我的声音」最直接的一条 */
        const esp_xiaozhi_chat_text_data_t *td = (const esp_xiaozhi_chat_text_data_t *)event_data;
        if (td != NULL && td->text != NULL) {
            s_reply_deadline = 0;       /* 服务端有回话，不算哑会话 */
            snprintf(s_last_text, sizeof(s_last_text), "%s", td->text);
            ESP_LOGI(TAG, "%s：%s",
                     (td->role == ESP_XIAOZHI_CHAT_TEXT_ROLE_USER) ? "我说" : "AI 答",
                     td->text);
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SYSTEM_CMD:
        /* 服务端可以让设备重启/升级；第一版只记录，不执行 */
        ESP_LOGW(TAG, "服务端指令：%s（忽略）",
                 (event_data != NULL) ? (const char *)event_data : "?");
        break;
    default:
        break;
    }
}

/* ===================== 会话流程 ===================== */

/**
 * @brief 协议层的连接状态（esp_event，不是音频事件回调）
 *
 * 组件在 MQTT 断开、或服务端下发 goodbye 时只 post 事件，不会自己重建
 * 音频通道。以前这里没监听，断线后状态还停在 READY —— 用户按住说话
 * 发出去的数据全部落空，界面看起来就是"怎么按都不回"。
 * 这里只置标志，真正的重连放在 ai_task 里做（事件回调在别的任务上下文）。
 */
static void ai_link_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base != ESP_XIAOZHI_CHAT_EVENTS) {
        return;
    }
    if (id == ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "与服务器断开");
        s_link_lost = true;
    } else if (id == ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE) {
        ESP_LOGW(TAG, "服务端结束会话");
        s_link_lost = true;
    }
}

/**
 * @brief 建一次完整会话：MCP → chat 实例 → 传输连接 → 音频通道
 */
static bool session_build(void)
{
    esp_mcp_t *mcp = NULL;

    /* MCP engine 是组件强制要求的（内部用它做传输层），同时把设备工具挂上去 */
    if (esp_mcp_create(&mcp) != ESP_OK || mcp == NULL) {
        set_state(SRV_AI_ERROR, "MCP 初始化失败");
        return false;
    }
    srv_ai_tools_register(mcp);

    esp_xiaozhi_chat_config_t cfg = {
        .audio_type           = ESP_XIAOZHI_CHAT_AUDIO_TYPE_OPUS,
        .audio_callback       = ai_audio_cb,
        .event_callback       = ai_event_cb,
        .mcp_engine           = mcp,
        .owns_mcp_engine      = true,
        .has_mqtt_config      = s_info.has_mqtt_config,
        .has_websocket_config = s_info.has_websocket_config,
    };

    if (esp_xiaozhi_chat_init(&cfg, &s_chat) != ESP_OK) {
        /* 初始化失败时组件内部可能已经收尾过 mcp，这里不再重复释放 */
        s_chat = 0;
        set_state(SRV_AI_ERROR, "会话初始化失败");
        return false;
    }

    if (esp_xiaozhi_chat_start(s_chat) != ESP_OK) {
        set_state(SRV_AI_ERROR, "连接服务器失败");
        return false;
    }

    /* 上行按 hello 的 format 解析，用 PCM；下行 TTS 仍是 Opus，由解码器处理 */
    esp_xiaozhi_chat_audio_t audio = {
        .format         = "pcm",
        .sample_rate    = AI_SAMPLE_RATE,
        .channels       = 1,
        .frame_duration = AI_FRAME_MS,
    };
    if (esp_xiaozhi_chat_open_audio_channel(s_chat, &audio, NULL, 0) != ESP_OK) {
        set_state(SRV_AI_ERROR, "音频通道打开失败");
        return false;
    }
    return true;
}

/**
 * @brief 收掉当前会话（不再回调音频），下次重连重新建
 */
static void session_drop(void)
{
    if (s_chat != 0) {
        (void)esp_xiaozhi_chat_close_audio_channel(s_chat);
        (void)esp_xiaozhi_chat_stop(s_chat);
        (void)esp_xiaozhi_chat_deinit(s_chat);
        s_chat = 0;
    }
}


/**
 * @brief 等 WiFi 连上（最多 20 秒）
 */
static bool wait_wifi(void)
{
    for (int i = 0; i < 200 && !s_stop_req; i++) {
        if (srv_wifi_is_connected()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

/**
 * @brief 保证系统时间可用于 TLS 证书校验
 *
 * 板子刚上电时系统时间可能是 1970 年，HTTPS 会直接握手失败。
 */
static void ensure_time(void)
{
    time_t now = time(NULL);
    if (now > 1700000000) {         /* 2023-11 之后都算合理 */
        return;
    }
    ESP_LOGI(TAG, "系统时间过早（%ld），先校一次时", (long)now);
    (void)srv_ntp_sync_now(8000);
}

/**
 * @brief 获取服务端信息，必要时等用户绑定激活码
 * @return true = 已激活；false = 失败或被叫停
 */
static bool fetch_info_until_activated(void)
{
    for (;;) {
        memset(&s_info, 0, sizeof(s_info));
        esp_err_t ret = esp_xiaozhi_chat_get_info(&s_info);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "获取设备信息失败: %s", esp_err_to_name(ret));
            set_state(SRV_AI_ERROR, "连不上小智服务");
            return false;
        }

        if (!s_info.has_activation_code) {
            return true;
        }

        snprintf(s_act_code, sizeof(s_act_code), "%s",
                 (s_info.activation_code != NULL) ? s_info.activation_code : "");
        set_state(SRV_AI_ACTIVATING, "请在控制台输入激活码");
        ESP_LOGW(TAG, "待绑定，激活码 %s", s_act_code);

        esp_xiaozhi_chat_free_info(&s_info);

        /* 等 3 秒再拉一次；用户绑定成功后这里会拿不到 activation_code */
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (s_stop_req) {
            return false;
        }
    }
}

static void ai_task(void *arg)
{
    (void)arg;

    bool              listening = false;
    esp_opus_dec_cfg_t dcfg = {0};

    set_state(SRV_AI_CONNECTING, "正在连接…");

    if (!wait_wifi()) {
        set_state(SRV_AI_ERROR, s_stop_req ? "已停止" : "WiFi 未连接");
        goto cleanup;
    }

    ensure_time();

    if (!fetch_info_until_activated()) {
        goto cleanup;
    }

    s_act_code[0] = '\0';

    /* ---- 打开 Opus 解码器与缓冲（只建一次，断线重连时复用） ---- */
    s_pcm_in  = big_malloc(AI_TX_BYTES);
    s_dec_out = big_malloc(AI_DEC_PCM_MAX);
    if (s_pcm_in == NULL || s_dec_out == NULL) {
        set_state(SRV_AI_ERROR, "音频缓冲不足");
        goto cleanup;
    }

    dcfg.sample_rate    = AI_TTS_RATE;
    dcfg.channel        = 1;
    dcfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_INVALID;  /* 自动，按 60ms 估输出 */
    dcfg.self_delimited = false;
    if (esp_opus_dec_open(&dcfg, sizeof(dcfg), &s_dec) != ESP_AUDIO_ERR_OK || s_dec == NULL) {
        s_dec = NULL;
        set_state(SRV_AI_ERROR, "Opus 解码器初始化失败");
        goto cleanup;
    }

    (void)esp_event_handler_register(ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID,
                                     ai_link_event_cb, NULL);

    /* ---- 会话生命周期：断开就自动重建 ---- */
    while (!s_stop_req) {
        if (!session_build()) {
            session_drop();
            if (s_stop_req) {
                break;
            }
            set_state(SRV_AI_CONNECTING, "连接失败，5 秒后重试");
            for (int i = 0; i < 50 && !s_stop_req; i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }

        s_link_lost      = false;
        s_talk_req       = false;
        s_abort_req      = false;
        s_reply_deadline = 0;
        listening        = false;
        int64_t last_audio = esp_timer_get_time();
        set_state(SRV_AI_READY, "按住说话");

        /* ---- 会话内主循环：按需上行麦克风 ---- */
        while (!s_stop_req && !s_link_lost) {
            /* 兜底：说完话 15 秒还没有任何回应，说明会话已经哑了
             * （服务端会话过期 / UDP 映射失效，但 MQTT 没断线时收不到事件）。
             * 不重建的话就是"怎么按都不回"，所以直接当断线处理。 */
            if (s_reply_deadline != 0 && esp_timer_get_time() > s_reply_deadline) {
                s_reply_deadline = 0;
                ESP_LOGW(TAG, "说完话 15 秒没有回应，重建会话");
                s_link_lost = true;
                break;
            }
            /* 用户在 AI 回答时按下说话：先让服务端停止 TTS，再关喇叭、开麦 */
            if (s_abort_req) {
                s_abort_req = false;
                (void)esp_xiaozhi_chat_send_abort_speaking(
                    s_chat, ESP_XIAOZHI_CHAT_ABORT_SPEAKING_REASON_STOP_LISTENING);
                ESP_LOGI(TAG, "用户打断 AI 回答");
            }

            if (s_talk_req && !listening) {
                /* ES8311 的 DAC / ADC 共用同一组 I2S 时钟：上一轮 TTS 播放时
                 * 把 I2S 配成了 24k，不关掉的话麦克风实际按 24k 采样、却按
                 * 16k 的节奏发给服务端 —— 平台听起来就是"被慢放"。
                 * 先关扬声器通道，让 I2S 完全按麦克风的 16k 配置跑。 */
                if (s_spk_open) {
                    srv_audio_pcm_close();
                    s_spk_open = false;
                }

                if (srv_audio_mic_open(AI_SAMPLE_RATE, 1, 16) == ESP_OK) {
                    (void)esp_xiaozhi_chat_send_start_listening(
                        s_chat, ESP_XIAOZHI_CHAT_LISTENING_MODE_MANUAL);
                    listening = true;
                    set_state(SRV_AI_LISTENING, "正在听…");
                } else {
                    /* 麦克风被录音机占着：别死循环重试，提示一下 */
                    s_talk_req = false;
                    set_state(SRV_AI_ERROR, "麦克风被占用");
                }
            }

            if (!s_talk_req && listening) {
                srv_audio_mic_close();
                (void)esp_xiaozhi_chat_send_stop_listening(s_chat);
                listening = false;
                set_state(SRV_AI_READY, "按住说话");
            }

            if (listening) {
                int ret = srv_audio_mic_read(s_pcm_in, AI_TX_BYTES);
                if (ret == 0) {
                    (void)esp_xiaozhi_chat_send_audio_data(
                        s_chat, (const char *)s_pcm_in, AI_TX_BYTES);
                    last_audio = esp_timer_get_time();
                } else {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            } else {
                /* 空闲保活：服务端长时间收不到音频会主动 goodbye，
                 * 补一帧静音把它喂住，同一会话的上下文才不会断。 */
                if (esp_timer_get_time() - last_audio > AI_KEEPALIVE_US) {
                    memset(s_pcm_in, 0, AI_TX_BYTES);
                    if (esp_xiaozhi_chat_send_audio_data(
                            s_chat, (const char *)s_pcm_in, AI_TX_BYTES) == ESP_OK) {
                        ESP_LOGD(TAG, "空闲保活：补一帧静音");
                    }
                    last_audio = esp_timer_get_time();  /* 失败也等下一轮，别刷屏 */
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }

        if (listening) {
            srv_audio_mic_close();
            (void)esp_xiaozhi_chat_send_stop_listening(s_chat);
            listening = false;
        }
        if (s_spk_open) {
            srv_audio_pcm_close();
            s_spk_open = false;
        }
        session_drop();

        if (!s_stop_req) {
            /* 断开后别立刻重建：WiFi/MQTT 需要一点时间恢复，否则又连不上 */
            set_state(SRV_AI_CONNECTING, "连接断开，正在重连…");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

cleanup:
    /* 收尾：先停通道（不再回调）→ 关编解码器 → 关设备 */
    (void)esp_event_handler_unregister(ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID,
                                       ai_link_event_cb);
    if (srv_audio_mic_is_open()) {
        srv_audio_mic_close();
    }
    session_drop();
    if (s_dec != NULL) {
        esp_opus_dec_close(s_dec);
        s_dec = NULL;
    }
    free(s_pcm_in);
    free(s_dec_out);
    s_pcm_in  = NULL;
    s_dec_out = NULL;

    if (s_spk_open) {
        srv_audio_pcm_close();
        s_spk_open = false;
    }
    esp_xiaozhi_chat_free_info(&s_info);

    s_act_code[0] = '\0';
    s_last_text[0] = '\0';
    s_talk_req = false;
    s_reply_deadline = 0;
    if (s_state != SRV_AI_ERROR) {
        set_state(SRV_AI_IDLE, "已断开");
    }

    s_task_alive = false;
    vTaskDelete(NULL);
}

/* ===================== 对外接口 ===================== */

esp_err_t srv_ai_start(void)
{
    if (s_task_alive) {
        return ESP_ERR_INVALID_STATE;   /* 上一轮还在收尾 */
    }

    /* 同一时刻只能一个出声：先把音乐和电台停掉 */
    if (srv_player_state() != SRV_PLAYER_IDLE) {
        (void)srv_player_stop();
    }
    if (srv_radio_state() != SRV_RADIO_IDLE) {
        (void)srv_radio_stop();
    }

    s_stop_req = false;
    s_talk_req = false;
    s_abort_req = false;
    s_link_lost = false;
    s_reply_deadline = 0;
    s_act_code[0] = '\0';
    set_state(SRV_AI_CONNECTING, "正在连接…");

    /* TLS 握手对内部 RAM 很敏感（分配失败就是 mbedtls_ssl_setup -0x8D），
     * 每次连接打一行，出问题时有据可查 */
    ESP_LOGI(TAG, "内存：内部 %u KB（最低 %u KB）/ PSRAM %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    /* 栈必须放内部 RAM：任务里会调 get_info（写 NVS），而 NVS 写会短暂关闭
     * flash cache —— 栈在 PSRAM 的任务这时访问栈就会崩，所以不能用
     * xTaskCreateWithCaps。32KB 是 Opus 编码器实测需要的量。 */
    s_task_alive = true;
    if (xTaskCreate(ai_task, "xiaozhi", AI_TASK_STACK, NULL, AI_TASK_PRIO, NULL) != pdPASS) {
        s_task_alive = false;
        set_state(SRV_AI_ERROR, "任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void srv_ai_stop(void)
{
    if (!s_task_alive) {
        return;
    }
    s_stop_req = true;
    s_talk_req = false;
    set_state(SRV_AI_IDLE, "正在断开…");
}

esp_err_t srv_ai_talk(bool on)
{
    if (on) {
        if (s_state != SRV_AI_READY && s_state != SRV_AI_LISTENING &&
            s_state != SRV_AI_SPEAKING) {
            return ESP_ERR_INVALID_STATE;
        }
        /* 回答中按下 = 打断：先让服务端停 TTS，任务里再关喇叭开麦 */
        if (s_state == SRV_AI_SPEAKING) {
            s_abort_req = true;
        }
        s_reply_deadline = 0;
        s_talk_req = true;
    } else {
        s_talk_req = false;
        /* 松手后开始等回应；15 秒内没有任何文本/TTS 就当作会话已死 */
        if (s_state == SRV_AI_LISTENING) {
            s_reply_deadline = esp_timer_get_time() + 15000000LL;
        }
    }
    return ESP_OK;
}
