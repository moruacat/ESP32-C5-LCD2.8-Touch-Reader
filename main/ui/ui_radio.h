#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 电台页（中国之声 · HLS 在线直播）
 *        播放/暂停/继续 + 停止 + 音量；退页时自动停止
 */
void ui_radio_create(lv_obj_t *root);

#ifdef __cplusplus
}
#endif