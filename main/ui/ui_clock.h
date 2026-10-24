#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 时钟伪装页（主页）
 *        大号时:分 + 秒、年月日星期、温湿度、状态栏（SD 图标 / 电池图标）
 * @param root 页面根容器
 */
void ui_clock_create(lv_obj_t *root);

#ifdef __cplusplus
}
#endif
