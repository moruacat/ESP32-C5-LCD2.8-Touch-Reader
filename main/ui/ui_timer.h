#pragma once
#include "lvgl.h"
#include <stdbool.h>

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

/**
 * @brief 计时器页是否启用了 BOOT 键启停
 */
bool ui_timer_boot_control_enabled(void);

/**
 * @brief 从 BOOT 键输入任务调用：启停秒表并刷新页面
 */
void ui_timer_boot_toggle(void);

#ifdef __cplusplus
}
#endif
