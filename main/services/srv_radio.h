#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 电台服务：HLS 在线广播（中国之声）
 *
 * 拉 m3u8 播放列表 → 按 MEDIA-SEQUENCE 增量取分片（TS/AAC）→ 软解成 PCM →
 * 送扬声器。直播流没有 ENDLIST，列表是个滑动窗口，所以要周期性重拉。
 *
 * 播放跑在独立任务里（拉流和 PCM 写入都会阻塞，不能搁在 UI 线程上），
 * 对外的 start/pause/resume/stop 都是投命令，状态靠轮询。
 *
 * 同一时刻音乐和电台只能有一个出声（共用 ES8311 通道），起播前会先停音乐。
 */

/*! 当前状态（UI 轮询用） */
typedef enum {
    SRV_RADIO_IDLE = 0,     /*!< 未播放（没起播，或已停止） */
    SRV_RADIO_BUFFERING,    /*!< 正在连接 / 缓冲 / 断线重试中 */
    SRV_RADIO_PLAYING,      /*!< 正在播放 */
    SRV_RADIO_PAUSED,       /*!< 已暂停 */
} srv_radio_state_t;

/*! 电台名（界面上显示用） */
#define SRV_RADIO_NAME   "中国之声"

/**
 * @brief 开始收听（直播：从当前时刻往前的窗口里起播）
 *        已经在播/缓冲时是空操作
 */
esp_err_t srv_radio_start(void);

/**
 * @brief 暂停（会断掉连接、释放扬声器通道；继续时重新对齐直播）
 */
esp_err_t srv_radio_pause(void);

esp_err_t srv_radio_resume(void);

/**
 * @brief 停止并释放所有资源（退出电台页时调用）
 */
esp_err_t srv_radio_stop(void);

srv_radio_state_t srv_radio_state(void);

/**
 * @brief 状态行文本（"正在连接…" / "正在播放 · 96 kbps" / "网络未连接" …）
 *        可直接贴到界面上
 */
const char *srv_radio_status_text(void);

#ifdef __cplusplus
}
#endif