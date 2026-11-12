#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 单词变形查询页（二级菜单）
 *
 * 输入任意形式（ran / better / studies），显示原形、音标、释义和全部变形。
 */
void ui_word_forms_create(lv_obj_t *root);

#ifdef __cplusplus
}
#endif
