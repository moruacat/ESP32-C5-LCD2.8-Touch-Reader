#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 计时器页（正计时 / 秒表）
 *
 * 只负责显示与操作，计时状态保存在 srv_timer 里，
 * 所以退出页面后计时继续走，时钟页也能看到。
 */
void ui_timer_create(lv_obj_t *root);

#ifdef __cplusplus
}
#endif
