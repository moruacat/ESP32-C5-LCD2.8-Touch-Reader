#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! 单本正文上限（转成 UTF-8 之后的字节数）
 *  正文是整本读进 PSRAM 的，320x240 的小屏上没必要支持超大文件 */
#define SRV_READER_TEXT_MAX   (4 * 1024 * 1024)

typedef enum {
    READER_FMT_NONE = 0,    /*!< 没打开任何书 */
    READER_FMT_TXT,         /*!< 单个 .txt 文件 */
    READER_FMT_EBF,         /*!< EBF 目录：data.ebfl + chapter.N.ebfl */
} reader_format_t;

/**
 * @brief 打开一本书
 *
 * 传文件路径按 TXT 处理，传目录路径按 EBF 处理。
 * 正文整本读进 PSRAM，并统一转成 UTF-8（自动识别 UTF-8 / GBK）。
 *
 * 会阻塞到读完为止（SD 慢，几百毫秒起步），界面里请改用
 * srv_reader_open_async() 把它丢到后台。
 *
 * @return ESP_OK；ESP_ERR_NOT_FOUND 路径不存在；ESP_ERR_INVALID_SIZE 超过上限
 */
esp_err_t srv_reader_open(const char *path);

/* ===================== 后台打开 ===================== */

/**
 * @brief 后台打开一本书，立刻返回
 *
 * 数章节、读元数据、读第一章全在独立任务里做，期间界面可以照常刷新。
 * 打开中 srv_reader_busy() 为 true，此时不要调用本模块的其它查询接口
 * （它们读的正是任务在写的那份状态）。同一时刻只允许一个后台任务。
 *
 * @return ESP_OK 已开始；ESP_ERR_INVALID_STATE 上一个还没结束
 */
esp_err_t srv_reader_open_async(const char *path);

/*! 后台打开是否还在进行中 */
bool srv_reader_busy(void);

/*! 后台打开的结果；没发起过后台打开时返回 ESP_ERR_INVALID_STATE */
esp_err_t srv_reader_open_result(void);

/**
 * @brief 后台打开的进度，0-100
 *
 * 粗粒度分档：5 起步 → 12 元数据读完 → 20 章数确定 → 20~70 读第一章
 * （按读入字节数递增）→ 70~95 按上次进度切章 → 100 读完。原来是整章
 * 一次 fread，进度条只会从头跳到尾，分块之后才真的在动。
 */
int srv_reader_open_progress(void);

/**
 * @brief 打开过程中正在读的那个文件的字节数（原始字节，不是 KB）
 *
 * 打开流程可能读两次正文（第一章、按上次进度切到的那一章），每次都会
 * 重置这两个值 —— 它反映的是「手上这个章节读了多少」，界面拿它显示
 * 「已读 x KB / 共 y KB」。还没开始读正文时两个都是 0。
 *
 * @param done  已读字节，可为 NULL
 * @param total 文件总字节，可为 NULL
 */
void srv_reader_open_bytes(int *done, int *total);

/**
 * @brief 关闭当前书，释放正文内存
 */
void srv_reader_close(void);

bool srv_reader_is_open(void);

reader_format_t srv_reader_format(void);
const char     *srv_reader_title(void);
const char     *srv_reader_author(void);

/*! 当前打开的书籍路径（UI 拿它去 srv_library 查封面、简介这些元数据） */
const char     *srv_reader_path(void);

/*! 当前这一章的字符数（TXT 就是整本书）。按 Unicode 码点算，不是字节数 */
int srv_reader_char_count(void);

/* ===================== 章节 ===================== */

/*! EBF 的章节数；TXT 恒为 1 */
int srv_reader_chapter_count(void);

int srv_reader_chapter_index(void);

/**
 * @brief 取章节标题：优先用 data.ebfl 里的，没有就写"第 N 章"
 */
void srv_reader_chapter_title(int idx, char *buf, size_t buf_len);

esp_err_t srv_reader_goto_chapter(int idx);

/* ===================== 取文本 ===================== */

/**
 * @brief 从第 char_off 个字符起，读最多 char_cnt 个字符
 *
 * 输出是 UTF-8 且以 '\0' 结尾；读到章节末尾会少于 char_cnt，
 * 实际读到的个数写进 read_cnt。
 *
 * 分页交给调用方：这里只管按字符窗口取文本，具体一页能放多少字
 * 取决于字号、行距和屏幕宽高，那是渲染层才知道的事。
 */
esp_err_t srv_reader_read(int char_off, int char_cnt, char *out, size_t out_len, int *read_cnt);

/* ===================== 阅读位置 ===================== */

int srv_reader_position(void);

esp_err_t srv_reader_seek(int char_off);

/*! 保存/恢复进度（进度文件路径由 srv_progress 按书籍路径推导） */
esp_err_t srv_reader_save_progress(void);

/**
 * @brief 立即保存进度，不受节流限制
 *
 * 退出阅读器时用。翻页时的保存有最小写盘间隔（避免每翻一页就写 SD），
 * 被挡掉的那次如果不在这里补上，退出后就回到几页之前了。
 */
esp_err_t srv_reader_save_progress_force(void);

esp_err_t srv_reader_load_progress(void);

#ifdef __cplusplus
}
#endif
