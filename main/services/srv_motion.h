#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

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

/* ===================== 姿态 / 网络传输（电脑端 3D 显示用） ===================== */

/**
 * @brief 一帧姿态采样
 *
 * 角度单位：四元数无量纲，加速度 g（9.807 m/s² = 1.0），角速度 °/s。
 * 轴就是 QMI8658 的芯片轴（与板子的焊接方向有关，电脑端可以旋转对齐）。
 */
typedef struct {
    uint32_t seq;                               /*!< 递增序号，丢包统计用 */
    float    ax, ay, az;                        /*!< 加速度（g） */
    float    gx, gy, gz;                        /*!< 角速度（°/s） */
    float    qw, qx, qy, qz;                    /*!< 姿态四元数（机身 → 世界） */
} srv_motion_sample_t;

/**
 * @brief 读一帧并更新姿态融合（任意频率调用，内部按 dt 积分）
 *
 * 互补滤波：陀螺积分负责快速响应，加速度计负责修正长期漂移。
 * 上电后的第一批数据会用来初始化姿态。
 */
esp_err_t srv_motion_read_sample(srv_motion_sample_t *out);

/**
 * @brief 打开/关闭姿态 UDP 广播（端口 4210，30Hz，文本行）
 *
 * 同一 WiFi 下电脑运行 tools/motion3d 监听即可看到实时 3D 姿态。
 * 关闭时任务退出，不会占 CPU/网络。
 */
void srv_motion_stream_set_enabled(bool en);
bool srv_motion_stream_enabled(void);

/** 广播端口（固定；电脑端与固件要一致） */
#define SRV_MOTION_UDP_PORT     4210

/* ===================== 方向校正 ===================== */

/**
 * @brief 当前的三个方向开关
 *
 * 芯片实际的焊装朝向没法从代码推出来（这批板子实测是「X→屏幕横、Y→屏幕纵」），
 * 所以给三个开关兜底，合起来覆盖平面内全部 8 种正交朝向：
 *   swap_xy  交换两轴 —— 手感差了 90° 时用
 *   flip_x   左右翻转
 *   flip_y   上下翻转
 * 选择存在 NVS 配置的 motion_flip 里（bit2/bit0/bit1），重启保留。
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
