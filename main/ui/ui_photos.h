#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 照片页：列表 + 全屏横滑浏览（左右滑切换、轻点退出）
 *
 * 进入前的密码校验在 ui_func.c 里做，本页只负责浏览。
 */
void ui_photos_create(lv_obj_t *root);

#ifdef __cplusplus
}
#endif