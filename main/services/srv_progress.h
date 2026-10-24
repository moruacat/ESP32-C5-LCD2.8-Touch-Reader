#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== 阅读进度 ===================== */

/**
 * @brief 阅读进度
 *
 * 存储位置（新需求规范）：
 *   EBF（书籍是文件夹）-> <书目录>/progress.dat
 *   TXT（书籍是文件）  -> <书籍路径>.progress.dat
 */
typedef struct {
    int32_t chapter;        /*!< EBF 章节号；TXT 恒为 0 */
    int32_t page;           /*!< 当前页码（TXT 为页序号） */
    int32_t line_off;       /*!< 页内行偏移 */
    int32_t byte_off;       /*!< 文件字节偏移（TXT 断点续读用） */
    int32_t pct;            /*!< 阅读百分比 0-100 */
    int32_t font_size;      /*!< 保存时的字号，便于按同版式恢复 */
    int32_t line_space;     /*!< 保存时的行距 */
} srv_progress_reader_t;

/**
 * @brief 载入阅读进度
 * @return ESP_OK；文件缺失/损坏返回错误，调用方应回退到从头开始
 */
esp_err_t srv_progress_reader_load(const char *book_path, srv_progress_reader_t *out);

/**
 * @brief 保存阅读进度
 * @param force true = 立即落盘（退出阅读时用）；false = 5 秒内只写一次
 */
esp_err_t srv_progress_reader_save(const char *book_path, const srv_progress_reader_t *in, bool force);

/**
 * @brief 删除阅读进度
 */
esp_err_t srv_progress_reader_clear(const char *book_path);

/* ===================== 音乐播放进度 ===================== */

#define SRV_PROGRESS_FILE_MAX   256

/**
 * @brief 音乐播放进度：上次的列表、曲目、播放方式
 *        （不记播放位置：开机恢复时一律从头放）
 */
typedef struct {
    int32_t playlist;                       /*!< 播放列表序号 */
    int32_t playing;                        /*!< 1 = 上次处于播放中 */
    int32_t index;                          /*!< 列表内序号 */
    int32_t mode;                           /*!< 播放方式（srv_player_mode_t） */
    char    file[SRV_PROGRESS_FILE_MAX];    /*!< 音乐文件名 */
} srv_progress_music_t;

esp_err_t srv_progress_music_load(srv_progress_music_t *out);

/**
 * @brief 保存音乐进度
 * @param force 同 reader_save
 */
esp_err_t srv_progress_music_save(const srv_progress_music_t *in, bool force);

/**
 * @brief 音乐进度文件路径（便于日志与调试）
 */
const char *srv_progress_music_path(void);

#ifdef __cplusplus
}
#endif
