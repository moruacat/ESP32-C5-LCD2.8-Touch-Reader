#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化音频（I2S + ES8311），音量取自 /config.dat
 */
esp_err_t srv_audio_init(void);

/* ===================== 音量 ===================== */

/**
 * @brief 设置全局音量并落盘（0-100）
 */
esp_err_t srv_audio_set_volume(int vol);

int srv_audio_get_volume(void);

/* ===================== 闹钟音量 ===================== */

esp_err_t srv_audio_set_alarm_volume(int vol);

int srv_audio_get_alarm_volume(void);

/* ===================== 音乐时段静音 ===================== */

/**
 * @brief 当前 RTC 时间是否落在任一已启用的静音时段内
 */
bool srv_audio_in_mute_window(void);

/**
 * @brief 进音乐页时应用一次时段静音
 *
 * 落在静音时段内就把音乐音量设成 0%（写进配置），**之后不再干预** ——
 * 暂停/继续/切歌都不会再改音量，退出音乐页也不恢复。想恢复正常音量，
 * 要么手动拖音量条，要么等出了时段再重进音乐页。
 *
 * 只动音乐音量（CFG_VOLUME），闹钟/提示音是各自独立的音量，不受影响。
 *
 * @return true 表示本次把音量压到了 0
 */
bool srv_audio_apply_time_mute(void);

/* ===================== PCM 播放（音乐播放器用） ===================== */

/**
 * @brief 按指定参数打开扬声器通道
 *        音乐播放期间由播放任务独占，与提示音互斥
 */
esp_err_t srv_audio_pcm_open(int sample_rate, int channels, int bits);

/**
 * @brief 写一段 PCM（阻塞，直到写完或出错）
 * @return 实际写入字节数，负数表示失败
 */
int srv_audio_pcm_write(const void *data, int bytes);

/**
 * @brief 关闭扬声器通道
 */
void srv_audio_pcm_close(void);

bool srv_audio_pcm_is_open(void);

/* ===================== 提示音 ===================== */

/**
 * @brief 播放一段方波提示音（阻塞）
 * @param freq_hz     频率（如 2000）
 * @param duration_ms 时长
 */
esp_err_t srv_audio_beep(int freq_hz, int duration_ms);

#ifdef __cplusplus
}
#endif
