#pragma once
#include "esp_err.h"
#include "srv_fs.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== 容量与字段长度 ===================== */

#define SRV_MUSIC_MAX          400     /*!< 媒体库曲目上限 */
#define SRV_MUSIC_NAME_MAX     128     /*!< 显示名（不含扩展名） */
#define SRV_MUSIC_LIST_MAX     16      /*!< 播放列表个数上限（playlists.1..16.dat） */

/**
 * @brief 支持的音频格式（对应 esp_audio_simple_player 能解的编码）
 */
typedef enum {
    SRV_MUSIC_FMT_UNKNOWN = 0,
    SRV_MUSIC_FMT_MP3,
    SRV_MUSIC_FMT_WAV,
    SRV_MUSIC_FMT_AAC,
    SRV_MUSIC_FMT_M4A,
    SRV_MUSIC_FMT_FLAC,
    SRV_MUSIC_FMT_OGG,
    SRV_MUSIC_FMT_AMR,
    SRV_MUSIC_FMT_TS,
} srv_music_fmt_t;

/**
 * @brief 媒体库曲目
 */
typedef struct {
    srv_music_fmt_t fmt;                        /*!< 音频格式 */
    bool            external;                   /*!< true = 来自 /music/others.dat（外部曲目） */
    uint32_t        size;                       /*!< 文件字节数 */

    char            name[SRV_MUSIC_NAME_MAX];   /*!< 显示名（已去掉扩展名） */
    char            path[SRV_FS_PATH_MAX];      /*!< 绝对路径 */
} srv_music_t;

/* ===================== 媒体库 ===================== */

/**
 * @brief 初始化音乐库：建 /music 目录、扫描、准备播放列表
 */
esp_err_t srv_musiclib_init(void);

/**
 * @brief 重新扫描 /music/ 并合并 others.dat 中的外部曲目
 *        扫描成功但一条都没有也返回 ESP_OK
 */
esp_err_t srv_musiclib_scan(void);

/**
 * @brief 当前媒体库条数
 */
int srv_musiclib_count(void);

/**
 * @brief 按序号取曲目（顺序：按文件名排序后的 /music 曲目 + 外部曲目）
 * @return ESP_OK / ESP_ERR_INVALID_ARG
 */
esp_err_t srv_musiclib_get(int idx, const srv_music_t **out);

/**
 * @brief 按路径查曲目
 * @return ESP_OK / ESP_ERR_NOT_FOUND
 */
esp_err_t srv_musiclib_find(const char *path, const srv_music_t **out);

/**
 * @brief 把外部音频加入媒体库（记入 /music/others.dat，不移动源文件）
 */
esp_err_t srv_musiclib_add_external(const char *path);

/**
 * @brief 从媒体库移除记录（不删除源文件）
 */
esp_err_t srv_musiclib_remove(const char *path);

/**
 * @brief 媒体库根目录（<SD>/music）
 */
const char *srv_musiclib_root(void);

/**
 * @brief 外部曲目清单路径（<SD>/music/others.dat）
 */
const char *srv_musiclib_others_path(void);

/* ===================== 播放列表 ===================== */

/**
 * @brief 现有播放列表个数（按序号升序；一个都没有时返回 0）
 */
int srv_musiclib_playlist_count(void);

/**
 * @brief 第 slot 个播放列表的序号（slot: 0..count-1）
 * @return 序号（1..SRV_MUSIC_LIST_MAX）；slot 越界返回 0
 */
int srv_musiclib_playlist_id(int slot);

/**
 * @brief 新建一个空播放列表
 * @param out_id 新列表序号，可为 NULL
 * @return 没有空位时返回 ESP_ERR_NO_MEM
 */
esp_err_t srv_musiclib_playlist_create(int *out_id);

/**
 * @brief 删除一个播放列表（连同它的曲目清单文件）
 */
esp_err_t srv_musiclib_playlist_delete(int id);

/**
 * @brief 打开（载入内存）指定序号的播放列表，之后用下面的 item/append 系列操作
 *        文件不存在按空列表处理
 */
esp_err_t srv_musiclib_playlist_open(int id);

/**
 * @brief 当前已打开的播放列表序号（未打开为 0）
 */
int srv_musiclib_playlist_opened(void);

/**
 * @brief 已打开的播放列表条数
 */
int srv_musiclib_playlist_item_count(void);

/**
 * @brief 取已打开列表中的一条
 */
esp_err_t srv_musiclib_playlist_item(int idx, const srv_music_t **out);

/**
 * @brief 追加一条到已打开的列表（立即落盘）
 */
esp_err_t srv_musiclib_playlist_append(const char *path);

/**
 * @brief 从已打开的列表里删掉一条（立即落盘）
 */
esp_err_t srv_musiclib_playlist_remove_item(int idx);

/**
 * @brief 在已打开的列表里调整顺序（把 from 移到 to）
 */
esp_err_t srv_musiclib_playlist_move(int from, int to);

/**
 * @brief 清空已打开的列表（立即落盘）
 */
esp_err_t srv_musiclib_playlist_clear(void);

/**
 * @brief 播放列表文件路径（<SD>/music/playlists.<id>.dat）
 */
esp_err_t srv_musiclib_playlist_path(int id, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
