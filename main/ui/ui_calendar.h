#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建日历页面（时钟页上滑进入）
 *
 * 月历视图：日/一/二/三/四/五/六 七列，当月日期按星期对齐，今天用主题色
 * 打底；左右箭头翻月，底部「今天」跳回当前月。日期来源是 RTC。
 */
void ui_calendar_create(lv_obj_t *root);

#ifdef __cplusplus
}
#endif