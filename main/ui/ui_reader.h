#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建阅读器页面
 *
 * 进入前必须先调 srv_reader_open_async() 发起打开（先切页再等也行，
 * 页面上会显示「正在打开」）。断点续读靠 srv_progress，页表则在页面
 * 创建时按阅读位置重建（位置靠后时分片做，不卡界面）。
 * 每次翻页都会更新阅读位置并落盘（内部有节流）。
 *
 * 交互：点屏幕左侧翻上一页，右侧翻下一页，右滑返回书架。
 */
void ui_reader_create(lv_obj_t *root);

#ifdef __cplusplus
}
#endif
