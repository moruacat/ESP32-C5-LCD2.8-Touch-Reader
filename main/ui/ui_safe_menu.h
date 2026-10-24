#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建安全菜单页面
 *
 * 时钟页手势（左上角连点 5 次 + 2 秒内右滑）打开的页：左右两张大卡 ——
 * 左边进「功能菜单」（书架/音乐/游戏/照片/录音机），右边进「完整版设置」
 * （设置列表 + 安全模式开关，普通设置页里没有这个开关）。
 *
 * 页面本身不带任何「安全」字样，看着就是普通菜单。
 */
void ui_safe_menu_create(lv_obj_t *root);

#ifdef __cplusplus
}
#endif