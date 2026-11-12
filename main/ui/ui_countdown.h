#pragma once
#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 倒计时页（时/分/秒滚轮设定时长 + 到点屏幕提示）
 *
 * 到点提示音开关存在 /config.dat 的 timer_sound 项里，本页右下角按钮切换。
 */
void ui_countdown_create(lv_obj_t *root);

/**
 * @brief 倒计时页是否启用了 BOOT 键启停
 */
bool ui_countdown_boot_control_enabled(void);

/**
 * @brief 从 BOOT 键输入任务调用：启停倒计时并刷新页面
 *        （设定形态下先读取滚轮时长）
 */
void ui_countdown_boot_toggle(void);

#ifdef __cplusplus
}
#endif
