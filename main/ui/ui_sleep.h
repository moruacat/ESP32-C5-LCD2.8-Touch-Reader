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

/**
 * @brief 临时强制启用「点击屏幕唤醒」
 *
 * 只改运行时的覆盖标志，**不写入 CFG_TOUCH_WAKE**。计时器 / 倒计时的
 * BOOT 启停开关打开时使用，避免休眠后无法用触摸唤醒。
 *
 * @param owner 所有者标记（页面 root 指针）。关闭时只有 owner 匹配才会清除，
 *              避免切页时旧页面把新页面设置的覆盖误清掉。
 */
void ui_sleep_set_force_touch_wake(bool on, const void *owner);

#ifdef __cplusplus
}
#endif
