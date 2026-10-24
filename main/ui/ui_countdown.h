#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 倒计时页（时/分/秒滚轮设定时长 + 到点屏幕提示）
 *
 * 到点提示音开关存在 /config.dat 的 timer_sound 项里，本页右下角按钮切换。
 */
void ui_countdown_create(lv_obj_t *root);

#ifdef __cplusplus
}
#endif
