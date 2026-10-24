#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 贪吃蛇（20x13 格，滑动转向）
 */
void ui_game_snake_create(lv_obj_t *root);

#ifdef __cplusplus
}
#endif
