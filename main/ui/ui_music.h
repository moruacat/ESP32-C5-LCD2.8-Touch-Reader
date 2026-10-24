#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 音乐页：上方曲目列表 + 下方当前曲目 / 进度条 / 播放控制
 *        播放列表菜单（列表切换、播放方式、后台播放、添加曲目）在本页内弹层
 */
void ui_music_create(lv_obj_t *root);

#ifdef __cplusplus
}
#endif
