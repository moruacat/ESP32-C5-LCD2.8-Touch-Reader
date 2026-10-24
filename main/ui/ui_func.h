#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 功能菜单页：卡片风格，含书架 / 音乐 / 游戏 / AI
 * @param root 页面根容器
 */
void ui_func_create(lv_obj_t *root);

#ifdef __cplusplus
}
#endif
