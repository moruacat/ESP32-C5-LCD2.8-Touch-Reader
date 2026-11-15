#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 书名/作者与固件结构体对齐（服务端转换时已截断） */
#define SRV_CATALOG_NAME_MAX    192
#define SRV_CATALOG_AUTHOR_MAX  48
#define SRV_CATALOG_HASH_MAX    24
#define SRV_CATALOG_MAX         64      /*!< 一次最多列多少本 */

/**
 * @brief 服务端书单的一项
 */
typedef struct {
    int64_t id;                              /*!< 服务端 book id */
    char    title[SRV_CATALOG_NAME_MAX];
    char    author[SRV_CATALOG_AUTHOR_MAX];
    int     chapters;
    int64_t bytes;
    char    hash[SRV_CATALOG_HASH_MAX];      /*!< 整书内容哈希，用来判断是否有更新 */
} srv_catalog_book_t;

/**
 * @brief 拉取服务端书单（GET {server}/api/books）
 *
 * 需要先通过 srv_server 配好地址并连上 WiFi。
 *
 * @param out    输出数组（调用方用 srv_catalog_free 释放）
 * @param count  实际条数
 * @return ESP_OK / 网络或解析错误
 */
esp_err_t srv_catalog_fetch(srv_catalog_book_t **out, int *count);

/** @brief 释放 srv_catalog_fetch 返回的数组 */
void srv_catalog_free(srv_catalog_book_t *list);

/**
 * @brief 下载一本书到 SD 的 /books 目录（分章拉取）
 *
 * @param book        服务端书单项
 * @param progress_cb 进度回调（已完成章, 总章数），可为 NULL
 * @return ESP_OK / 错误
 */
typedef void (*srv_catalog_progress_cb_t)(int done, int total);
esp_err_t srv_catalog_download(const srv_catalog_book_t *book,
                               srv_catalog_progress_cb_t progress_cb);

/**
 * @brief 本地下载状态
 */
typedef enum {
    SRV_CATALOG_NONE = 0,      /*!< SD 上没有这本书 */
    SRV_CATALOG_UP_TO_DATE,    /*!< 已下载且章节数与服务端一致 */
    SRV_CATALOG_OUTDATED,      /*!< 已下载但服务端章节数变了（有更新） */
} srv_catalog_state_t;

/**
 * @brief 查这本书在 SD 上的状态
 *
 * 判断依据是本地 data.ebfl 里的 `<book chapters="N">` 与服务端书单的
 * chapters 是否一致 —— 章节数变了就认为有更新（番茄追更只追加，够用）。
 */
srv_catalog_state_t srv_catalog_state(const srv_catalog_book_t *book);

#ifdef __cplusplus
}
#endif
