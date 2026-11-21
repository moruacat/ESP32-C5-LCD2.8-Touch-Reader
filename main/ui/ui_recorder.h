#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 录音机页：录音/停止 + 回放 SD 卡 /record/rec-N.wav
 *
 * 列表只显示 rec-N.wav（按序号从大到小）；点一行用播放器的「单文件播放」
 * 试听，再点一下停止。录音中会先停录音再播放。
 */
void ui_recorder_create(lv_obj_t *root);

#ifdef __cplusplus
}
#endif
