#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 主菜单页：卡片风格
 *        卡片：功能 / 计时器 + 倒计时（上下分割共用一个卡片空间）/ 设置
 *        底部：返回键 + 亮度图标 + 亮度数值 + 亮度条
 * @param root 页面根容器
 */
void ui_home_create(lv_obj_t *root);

#ifdef __cplusplus
}
#endif
