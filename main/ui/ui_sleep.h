#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化休眠与自动变暗
 *
 * BOOT 键：正在响铃时优先用来停铃，其余情况当作「休眠 / 唤醒」开关。
 * 休眠 = 关背光 + 盖一层黑屏（点屏幕任意处即可唤醒）。
 *
 * 自动变暗：闲置一段时间（在「设置 → 亮度」里可调，也能整个关掉）就把背光按
 * 比例压低（相对用户设置的亮度，最低不低于 UI_BRIGHTNESS_MIN），一有触摸立刻恢复。
 *
 * 要在 LVGL（bsp_display_start / app_navigator_init）之后调用。
 */
void ui_sleep_init(void);

/**
 * @brief 切换休眠状态（关屏 / 恢复配置里的亮度）
 */
void ui_sleep_set(bool on);

bool ui_sleep_is_on(void);

#ifdef __cplusplus
}
#endif
