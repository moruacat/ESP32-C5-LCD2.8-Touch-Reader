#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化录音服务（挂上 ES8311 的录音通路）
 *
 * 依赖 srv_audio_init() 建好的 I2S（片上是全双工的），所以要在它之后调用。
 */
esp_err_t srv_recorder_init(void);

/**
 * @brief 开始录音
 *
 * 文件自动命名成 `/record/rec-<序号>.wav`（序号取目录里已有的最大值 +1），
 * 目录不存在会自动创建。单个文件最长一小时，到点自动切到下一个序号继续录
 * （录音不中断）。停止后 wav 头会被回填成真实长度。
 */
esp_err_t srv_recorder_start(void);

/**
 * @brief 停止录音（等任务把文件收尾写完再返回）
 */
esp_err_t srv_recorder_stop(void);

bool srv_recorder_is_active(void);

/**
 * @brief 已录时长（毫秒）；没在录时返回上一次的时长
 */
int64_t srv_recorder_elapsed_ms(void);

/**
 * @brief 当前（或最近一次）的录音文件完整路径，从未录过返回空串
 */
const char *srv_recorder_last_file(void);

/* ===================== 录音文件列表（录音机页回放用） ===================== */

#define SRV_REC_MAX_FILES   32

/**
 * @brief 扫一遍 /record，把 rec-N.wav 按序号从大到小（最新的在前）收集起来
 *
 * 只扫目录不读文件内容，几十个文件也就几毫秒；页面每次进入/录音结束调一次，
 * 不要放到轮询定时器里。
 *
 * @return 找到的文件数（0 表示没有或 SD 未挂载）
 */
int srv_recorder_scan_files(void);

/**
 * @brief 取扫描结果里的第 idx 条
 *
 * @param path      输出完整路径（可以为 NULL）
 * @param path_len  路径缓冲大小
 * @param name      输出文件名（可以为 NULL）
 * @param name_len  文件名缓冲大小
 * @param bytes     输出文件字节数（可以为 NULL）
 * @return ESP_OK / ESP_ERR_INVALID_ARG（没扫描过或下标越界）
 */
esp_err_t srv_recorder_file_info(int idx, char *path, size_t path_len,
                                 char *name, size_t name_len, uint32_t *bytes);

#ifdef __cplusplus
}
#endif
