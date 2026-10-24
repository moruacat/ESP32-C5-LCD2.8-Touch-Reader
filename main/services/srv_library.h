#pragma once
#include "esp_err.h"
#include "srv_fs.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 书架容量与各字段长度 */
#define SRV_LIBRARY_MAX        48
#define SRV_BOOK_NAME_MAX      192
#define SRV_BOOK_AUTHOR_MAX    48
#define SRV_BOOK_INTRO_MAX     192

/**
 * @brief 书籍类型
 */
typedef enum {
    BOOK_TYPE_TXT = 0,      /*!< 单个 .txt 文件 */
    BOOK_TYPE_EBF,          /*!< .ebf 文件夹（chapter.N.ebfl + data.ebfl + cover.raw） */
} book_type_t;

/**
 * @brief 书籍条目
 */
typedef struct {
    book_type_t type;
    bool        external;               /*!< true = 来自 /books/others.dat（书架外部书籍） */
    int32_t     chapter_count;          /*!< EBF 章节数（chapter.N.ebfl 的个数）；TXT 为 0 */
    int32_t     cover_w;                /*!< EBF 封面宽（0 = 未知） */
    int32_t     cover_h;                /*!< EBF 封面高（0 = 未知） */
    int32_t     cover_bytes;            /*!< EBF cover.raw 字节数（0 = 无封面） */

    char        name[SRV_BOOK_NAME_MAX];      /*!< 书名（已去掉 .txt / .ebf 后缀） */
    char        path[SRV_FS_PATH_MAX];        /*!< 绝对路径 */
    char        author[SRV_BOOK_AUTHOR_MAX];  /*!< 作者（EBF 来自 data.ebfl，TXT 为空） */
    char        intro[SRV_BOOK_INTRO_MAX];    /*!< 简介 */
} srv_book_t;

/**
 * @brief 初始化书架：建 /books 目录、载入外部书籍清单、扫描一次
 */
esp_err_t srv_library_init(void);

/**
 * @brief 重新扫描 /books/ 并合并 others.dat 中的外部书籍
 *        扫描成功但一条都没有也返回 ESP_OK
 */
esp_err_t srv_library_scan(void);

/**
 * @brief 当前书架条数
 */
int srv_library_count(void);

/**
 * @brief 按序号取书籍
 * @return ESP_OK / ESP_ERR_INVALID_ARG
 */
esp_err_t srv_library_get(int idx, const srv_book_t **out);

/**
 * @brief 按路径查书籍
 * @return ESP_OK / ESP_ERR_NOT_FOUND
 */
esp_err_t srv_library_find(const char *path, const srv_book_t **out);

/**
 * @brief 把外部书籍加入书架（记入 /books/others.dat，不移动源文件）
 * @param path 绝对路径，指向 .txt 文件或 .ebf 文件夹
 */
esp_err_t srv_library_add_external(const char *path);

/**
 * @brief 把书籍移出书架（仅移除记录，不删除源文件）
 *        仅对 external 书籍有意义；/books 下的书籍会从 others.dat 里清除标记
 */
esp_err_t srv_library_remove(const char *path);

/**
 * @brief 读取 EBF 封面（cover.raw，RGB565 原始数据）
 *
 * 成功时 *out_data 指向**调用方负责 free()** 的缓冲（优先 PSRAM），
 * 宽高取自 data.ebfl；元数据尺寸与实际字节数对不上时按字节数重算高。
 *
 * @return ESP_OK / ESP_ERR_INVALID_ARG（不是 EBF）/ ESP_ERR_NOT_FOUND（无封面）
 *         / ESP_ERR_INVALID_SIZE（尺寸异常）/ ESP_ERR_NO_MEM / ESP_FAIL
 */
esp_err_t srv_library_read_cover(const srv_book_t *b, uint8_t **out_data,
                                 size_t *out_bytes, int32_t *out_w, int32_t *out_h);

/**
 * @brief 读取 EBF 简介全文（data.ebfl 里的 <intro>）
 *
 * 与扫描时存进 srv_book_t.intro 的内容不同：这里不限于前 4KB，也不按
 * 192 字节截断，所以"查看简介"能看到完整内容。
 *
 * @return ESP_OK / ESP_ERR_INVALID_ARG / ESP_ERR_NOT_FOUND
 */
esp_err_t srv_library_read_intro(const srv_book_t *b, char *out, size_t out_len);

/**
 * @brief 书架根目录（<SD>/books）
 */
const char *srv_library_root(void);

/**
 * @brief 外部书籍清单文件路径（<SD>/books/others.dat）
 */
const char *srv_library_others_path(void);

#ifdef __cplusplus
}
#endif
