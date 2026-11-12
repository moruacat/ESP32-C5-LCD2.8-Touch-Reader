#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 单词学习（选单元 → 左滑认识 / 右滑不认识的卡片）
 */
void ui_word_study_create(lv_obj_t *root);

#ifdef __cplusplus
}
#endif
