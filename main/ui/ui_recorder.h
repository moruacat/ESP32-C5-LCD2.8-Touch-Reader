#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 录音机页（录音/停止，文件存到 SD 卡的 /record/rec-N.wav）
 */
void ui_recorder_create(lv_obj_t *root);

#ifdef __cplusplus
}
#endif
