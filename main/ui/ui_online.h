#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 在线书库页：显示服务端地址、发现服务端、拉取书单
 *
 * 从书架标题栏的「在线」按钮进入。
 * 书单里的下载功能在 srv_catalog 里（分章拉取，尚未实现）。
 */
void ui_online_create(lv_obj_t *root);

#ifdef __cplusplus
}
#endif
