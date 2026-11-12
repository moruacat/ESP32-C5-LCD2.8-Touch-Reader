#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 打开运行时旋转验证层
 *
 * 用于真机确认 ST7789 MADCTL、LVGL 逻辑分辨率和触摸坐标映射。
 * 层内提供 0/90/180/270 四个方向按钮；20 秒无操作会自动恢复横屏并
 * 返回上一页，防止方向/触摸异常时无法退出。
 */
void ui_rotation_test_show(void);

#ifdef __cplusplus
}
#endif
