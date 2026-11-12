#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 小智 AI 语音对话（xiaozhi.me）
 *
 * 用官方 esp_xiaozhi 组件做协议层（WebSocket/MQTT + MCP + 音频通道），
 * 这里只负责三件事：设备激活流程、麦克风上行、TTS 下行送喇叭。
 *
 * 第一版是按键说话（PTT），没有离线唤醒词：
 *   srv_ai_talk(true)  按住 → 开麦并把 PCM 一帧帧发上去
 *   srv_ai_talk(false) 松手 → 停麦并告诉服务端「说完了」
 *
 * 与音乐/电台共用 ES8311，起会话前会先把它们停掉。
 */

typedef enum {
    SRV_AI_IDLE = 0,        /*!< 未启动 */
    SRV_AI_CONNECTING,      /*!< 联网 / 获取服务端信息 */
    SRV_AI_ACTIVATING,      /*!< 等待在控制台输入激活码 */
    SRV_AI_READY,           /*!< 会话就绪，等用户说话 */
    SRV_AI_LISTENING,       /*!< 正在上行麦克风 */
    SRV_AI_SPEAKING,        /*!< 正在播放 TTS */
    SRV_AI_ERROR,           /*!< 出错（看 srv_ai_state_text） */
} srv_ai_state_t;

/**
 * @brief 启动会话（后台任务，立即返回）
 *
 * 会先停掉音乐和电台；WiFi 没连上时直接进入错误状态。
 */
esp_err_t srv_ai_start(void);

/**
 * @brief 请求结束会话（异步：任务自己收尾，不阻塞调用方）
 */
void srv_ai_stop(void);

/**
 * @brief 会话任务是否还在跑（退出中也是 true）
 */
bool srv_ai_running(void);

srv_ai_state_t srv_ai_state(void);

/**
 * @brief 状态文本（可直接贴到界面上）
 */
const char *srv_ai_state_text(void);

/**
 * @brief 待绑定的 6 位激活码；已激活或还没拿到时是空串
 */
const char *srv_ai_activation_code(void);

/**
 * @brief 最近一句对话文本（用户的识别结果或 AI 的回复）；没有时是空串
 */
const char *srv_ai_last_text(void);

/**
 * @brief 按住/松开说话（READY / LISTENING 状态下有效）
 *
 * 在 AI 回答（SPEAKING）时按下表示打断：会通知服务端停止 TTS，
 * 关掉扬声器后再开麦 —— 本机没有回声消除，不能边放边听。
 */
esp_err_t srv_ai_talk(bool on);

#ifdef __cplusplus
}
#endif
