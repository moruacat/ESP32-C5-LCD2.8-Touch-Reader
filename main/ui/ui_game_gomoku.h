#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 五子棋（15x15，同屏双人轮流落子）
 */
void ui_game_gomoku_create(lv_obj_t *root);

#ifdef __cplusplus
}
#endif
