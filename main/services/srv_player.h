#pragma once
#include "esp_err.h"
#include "srv_musiclib.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 播放方式（互斥四种）
 *
 * 「全部循环」与「列表循环」的区别在播放队列来源：
 *   全部循环 -> 媒体库（/music 下所有曲目 + 外部曲目）
 *   其余三种 -> 当前播放列表
 */
typedef enum {
    SRV_PLAYER_MODE_ALL_LOOP = 0,   /*!< 全部循环：队列 = 媒体库全部，播完回到第一首 */
    SRV_PLAYER_MODE_LIST_LOOP,      /*!< 列表循环：队列 = 当前播放列表，播完回到第一首 */
    SRV_PLAYER_MODE_SINGLE,         /*!< 单曲循环：重复当前曲目 */
    SRV_PLAYER_MODE_SHUFFLE,        /*!< 随机播放：队列 = 当前播放列表，随机选下一首 */
    SRV_PLAYER_MODE_NUM,
} srv_player_mode_t;

typedef enum {
    SRV_PLAYER_IDLE = 0,            /*!< 停止 */
    SRV_PLAYER_PLAYING,
    SRV_PLAYER_PAUSED,
} srv_player_state_t;

/* ===================== 生命周期 ===================== */

/**
 * @brief 初始化播放器：注册解码器、建播放任务
 */
esp_err_t srv_player_init(void);

/* ===================== 播放控制 ===================== */

/**
 * @brief 打开某个播放列表并从第 index 首开始播放
 */
esp_err_t srv_player_play_playlist(int list_id, int index);

/**
 * @brief 切换当前播放列表（正在播放的曲目会停止）
 */
esp_err_t srv_player_set_playlist(int list_id);

/**
 * @brief 播放当前队列的第 index 首
 */
esp_err_t srv_player_play_index(int index);

/**
 * @brief 在「播放 / 暂停」之间切换（停止状态下等同于开始播放当前曲目）
 */
esp_err_t srv_player_toggle(void);

esp_err_t srv_player_pause(void);
esp_err_t srv_player_resume(void);

/**
 * @brief 停止播放并释放音频通道（退出后台播放时用）
 */
esp_err_t srv_player_stop(void);

esp_err_t srv_player_next(void);
esp_err_t srv_player_prev(void);

/**
 * @brief 跳转到当前曲目的指定位置（毫秒）
 */
esp_err_t srv_player_seek_ms(int ms);

/* ===================== 状态查询（UI 轮询用） ===================== */

srv_player_state_t srv_player_state(void);

/**
 * @brief 当前曲目；未播放任何曲目时为 NULL
 */
const srv_music_t *srv_player_current(void);

/**
 * @brief 当前曲目在队列中的序号（无曲目为 -1）
 */
int srv_player_index(void);

/**
 * @brief 当前播放队列长度（随播放方式变化）
 */
int srv_player_queue_count(void);

/**
 * @brief 取队列中的第 idx 条
 */
esp_err_t srv_player_queue_item(int idx, const srv_music_t **out);

/**
 * @brief 当前播放列表序号（未使用播放列表时为 0）
 */
int srv_player_playlist_id(void);

int srv_player_position_ms(void);
int srv_player_duration_ms(void);

/* ===================== 播放方式 ===================== */

/**
 * @brief 切换播放方式（会按当前曲目路径在新队列里重新定位）
 */
void srv_player_set_mode(srv_player_mode_t mode);

srv_player_mode_t srv_player_mode(void);

/* ===================== 进度 ===================== */

/**
 * @brief 保存上次的列表 / 曲目 / 播放位置
 * @param force true = 立即落盘
 */
esp_err_t srv_player_save_progress(bool force);

/**
 * @brief 恢复上次的列表 / 曲目 / 位置（只恢复状态，不自动出声）
 */
esp_err_t srv_player_restore(void);

#ifdef __cplusplus
}
#endif
