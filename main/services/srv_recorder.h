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
 * 目录不存在会自动创建。停止或写满上限后 wav 头会被回填成真实长度。
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

#ifdef __cplusplus
}
#endif
