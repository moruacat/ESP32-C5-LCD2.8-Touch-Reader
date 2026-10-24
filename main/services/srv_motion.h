#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化重力感应（复用 srv_sensors 已初始化的 QMI8658）
 *
 * 必须在 srv_sensors_init() 之后调用。传感器没起来时返回错误，
 * 但只是让重力类功能不可用，不影响其余部分。
 */
esp_err_t srv_motion_init(void);

/**
 * @brief 重力感应是否可用
 */
bool srv_motion_ready(void);

/**
 * @brief 读倾斜量，已映射到屏幕坐标系并做过低通滤波
 *
 * 输出单位是 g（1.0 = 完全竖直，0 = 完全水平），在 [-1, 1] 之间。
 *   *x > 0：机身右侧压低（球应向右滚）
 *   *y > 0：机身下侧压低（球应向下滚）
 *
 * 板子横放时两个轴都在 0 附近。滤波是有意为之：加速度计噪声不小，
 * 原始值直接拿去驱动小球会抖得看不清。
 *
 * @return ESP_OK；传感器不可用时返回 ESP_ERR_INVALID_STATE
 */
esp_err_t srv_motion_read_tilt(float *x, float *y);

/* ===================== 方向校正 ===================== */

/**
 * @brief 当前的三个方向开关
 *
 * 芯片实际的焊装朝向没法从代码推出来（这批板子实测是「X→屏幕横、Y→屏幕纵」），
 * 所以给三个开关兜底，合起来覆盖平面内全部 8 种正交朝向：
 *   swap_xy  交换两轴 —— 手感差了 90° 时用
 *   flip_x   左右翻转
 *   flip_y   上下翻转
 * 选择存在 /config.dat 的 motion_flip 里（bit2/bit0/bit1），重启保留。
 */
bool srv_motion_swap_xy(void);
bool srv_motion_flip_x(void);
bool srv_motion_flip_y(void);

/**
 * @brief 设置三个开关并落盘
 */
void srv_motion_set_flip(bool swap_xy, bool flip_x, bool flip_y);

#ifdef __cplusplus
}
#endif
