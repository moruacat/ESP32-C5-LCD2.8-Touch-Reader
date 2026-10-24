#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 长文件名上限：FATFS_MAX_LFN(255) + 结束符 */
#define SRV_FS_NAME_MAX     256
/* 路径缓冲建议长度 */
#define SRV_FS_PATH_MAX     320

/**
 * @brief 目录项
 */
typedef struct {
    char     name[SRV_FS_NAME_MAX];  /*!< UTF-8 文件名（不含路径部分） */
    bool     is_dir;                 /*!< 是否目录 */
    uint32_t size;                   /*!< 文件字节数；目录为 0 */
} srv_fs_entry_t;

/**
 * @brief 目录遍历回调
 * @param entry 当前目录项
 * @param arg   调用方透传参数
 * @return false 立即停止遍历
 */
typedef bool (*srv_fs_list_cb_t)(const srv_fs_entry_t *entry, void *arg);

/* ===================== 查询 ===================== */

/**
 * @brief 遍历目录（自动跳过 "." 与 ".."），条目顺序由文件系统决定
 * @param path 绝对路径，如 "/sdcard/books"
 */
esp_err_t srv_fs_list(const char *path, srv_fs_list_cb_t cb, void *arg);

/**
 * @brief 取单个路径的信息
 */
esp_err_t srv_fs_stat(const char *path, srv_fs_entry_t *out);

/**
 * @brief 路径是否存在
 */
bool srv_fs_exists(const char *path);

/**
 * @brief 是否为目录（不存在时返回 false）
 */
bool srv_fs_is_dir(const char *path);

/* ===================== 修改 ===================== */

/**
 * @brief 创建目录（已存在视为成功）
 */
esp_err_t srv_fs_mkdir(const char *path);

/**
 * @brief 删除文件或空目录
 */
esp_err_t srv_fs_remove(const char *path);

/**
 * @brief 递归删除：目录连同里面的内容一起删；文件则直接删
 *
 * 给文件管理器用 —— EBF 书是文件夹，里面有章节文件和封面，普通 srv_fs_remove
 * 删不掉非空目录。**没有回收站，删了就没了**，调用方务必先做二次确认。
 */
esp_err_t srv_fs_remove_tree(const char *path);

/**
 * @brief 重命名 / 移动
 */
esp_err_t srv_fs_rename(const char *from, const char *to);

/* ===================== 路径工具 ===================== */

/**
 * @brief 拼接路径，自动处理重复的 '/'；结果超出缓冲返回 ESP_ERR_INVALID_SIZE
 */
esp_err_t srv_fs_join(const char *dir, const char *name, char *out, size_t out_len);

/**
 * @brief 取上级目录；已在根目录时输出原路径
 */
void srv_fs_parent(const char *path, char *out, size_t out_len);

/**
 * @brief 取文件名部分："a/b/c.txt" -> "c.txt"
 */
const char *srv_fs_basename(const char *path);

/**
 * @brief 取小写扩展名（不含点）："a/B.TXT" -> "txt"；无扩展名输出空串
 */
void srv_fs_ext(const char *path, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
