#pragma once
#include "esp_err.h"
#include "srv_fs.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! 照片规格：由 tools/photo2raw 转换出来的 .raw 固定是这么大 */
#define SRV_PHOTO_W         320
#define SRV_PHOTO_H         240
#define SRV_PHOTO_BYTES     (SRV_PHOTO_W * SRV_PHOTO_H * 2)

/*! 列表里最多显示多少张照片。滚轮只建一个对象，所以上限只受内存约束；
 *  条目表约 0.6KB/张，512 张约 300KB（PSRAM） */
#define SRV_PHOTO_MAX       512
/*! 文件名缓冲（FATFS 长文件名上限 + 结束符） */
#define SRV_PHOTO_NAME_MAX  SRV_FS_NAME_MAX

/**
 * @brief 照片条目
 */
typedef struct {
    char     name[SRV_PHOTO_NAME_MAX];  /*!< 文件名（含 .raw 后缀） */
    char     path[SRV_FS_PATH_MAX];     /*!< 绝对路径 */
    uint32_t size;                      /*!< 字节数（正常应为 SRV_PHOTO_BYTES） */
} srv_photo_t;

/**
 * @brief 初始化照片库：建 /photos 目录并扫描一次
 *        SD 未挂载时返回错误，但不阻塞启动
 */
esp_err_t srv_photo_init(void);

/**
 * @brief 重新扫描 /photos（SD 重新挂载后要调一次）
 *        扫描成功但一张都没有也返回 ESP_OK
 *
 * 会阻塞到扫完（内含 readdir + qsort）；界面里请改用 srv_photo_scan_async()。
 */
esp_err_t srv_photo_scan(void);

/**
 * @brief 是否正在扫描
 *
 * 扫描期间条目表正在被改写，别去查表或读图；界面看到 true 就先显示「扫描中」。
 */
bool srv_photo_busy(void);

/**
 * @brief 后台扫描：马上返回，扫完在扫描任务里回调 done_cb（可为 NULL）
 *
 * 进照片页时用它，免得在 LVGL 线程里 readdir + qsort 把界面冻住。
 * 回调跑在扫描任务的上下文，里面要碰 LVGL 得自己取锁或转 lv_async_call。
 *
 * @return ESP_OK / ESP_ERR_INVALID_STATE（已在扫）/ ESP_ERR_NO_MEM
 */
esp_err_t srv_photo_scan_async(void (*done_cb)(void));

/**
 * @brief 当前照片张数
 */
int srv_photo_count(void);

/**
 * @brief 按序号取照片
 * @return ESP_OK / ESP_ERR_INVALID_ARG
 */
esp_err_t srv_photo_get(int idx, const srv_photo_t **out);

/**
 * @brief 读取一张照片的像素数据（RGB565 小端，320x240）
 *
 * 成功时 *out_data 指向**调用方负责 free()** 的缓冲（优先 PSRAM）。
 * 文件大小不等于 SRV_PHOTO_BYTES 时按损坏处理，返回 ESP_ERR_INVALID_SIZE。
 *
 * @return ESP_OK / ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_SIZE / ESP_ERR_NO_MEM / ESP_FAIL
 */
esp_err_t srv_photo_load(int idx, uint8_t **out_data, size_t *out_bytes);

/**
 * @brief 照片根目录（<SD>/photos）
 */
const char *srv_photo_root(void);

#ifdef __cplusplus
}
#endif
