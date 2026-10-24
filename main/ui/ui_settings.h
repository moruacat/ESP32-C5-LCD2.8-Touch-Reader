#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 设置页：列表视图
 *        顺序：网络 / 时间 / 记录 / 闹钟 / 阅读 / 音乐 / 关于
 * @param root 页面根容器
 */
void ui_settings_create(lv_obj_t *root);

/**
 * @brief 完整版设置（安全菜单右侧入口）
 *
 * 与普通设置同一套列表，区别：**永远全部显示**（安全模式下也不收起来），
 * 且末尾多一行「安全模式」开关 —— 那是唯一能开关安全模式的地方。
 * 普通设置里没有这个开关。
 */
void ui_settings_full_create(lv_obj_t *root);

#ifdef __cplusplus
}
#endif
