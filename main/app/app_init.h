#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 系统启动编排入口（app_main 调用）
 *        顺序：NVS → LCD CS → CH32 IO 扩展 → SD → 传感器/RTC → LVGL 显示 → UI → BOOT 键
 */
void app_init_all(void);

#ifdef __cplusplus
}
#endif
