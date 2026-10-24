#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 画板（手指涂鸦，可换色 / 换粗细 / 清空）
 */
void ui_game_paint_create(lv_obj_t *root);

#ifdef __cplusplus
}
#endif
