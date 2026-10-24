#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 倒计时到点回调
 *
 * 在「响铃任务」上下文执行（已响完提示音），里面可以阻塞地取 LVGL 锁。
 * 注意不要在这里调用 srv_timer_* 的启停接口造成递归。
 */
typedef void (*srv_timer_done_cb_t)(void *arg);

/**
 * @brief 初始化计时器服务（创建倒计时到点用的 esp_timer）
 */
esp_err_t srv_timer_init(void);

/**
 * @brief 注册倒计时到点回调（可为 NULL）
 */
esp_err_t srv_timer_register_done_cb(srv_timer_done_cb_t cb, void *arg);

/* ===================== 提示音开关（存 /config.dat） ===================== */

bool srv_timer_sound_enabled(void);

/** @brief 设置提示音开关并立即落盘 */
void srv_timer_set_sound(bool on);

/* ===================== 秒表（正计时） ===================== */

/** @brief 开始 / 暂停（按当前状态取反） */
void srv_timer_sw_toggle(void);

/** @brief 归零并停止 */
void srv_timer_sw_reset(void);

bool srv_timer_sw_is_running(void);

/** @brief 是否「有内容」：运行中，或暂停后已走了一段（用于主界面显示） */
bool srv_timer_sw_is_active(void);

/** @brief 已走毫秒数 */
int64_t srv_timer_sw_elapsed_ms(void);

/* ===================== 倒计时 ===================== */

/**
 * @brief 设定倒计时时长（会停表并清零进度）
 * @param h 0-23，m/s 0-59；三者全为 0 时按 1 秒处理
 */
void srv_timer_cd_set_hms(int h, int m, int s);

/** @brief 当前设定的总时长（毫秒） */
int64_t srv_timer_cd_total_ms(void);

/** @brief 设定总时长（毫秒），会停表并清零进度 */
void srv_timer_cd_set_ms(int64_t ms);

/** @brief 开始 / 暂停 / 继续；到点结束后再调用则用原时长重新开始 */
void srv_timer_cd_toggle(void);

/** @brief 停止并清零，回到「设定时长」状态 */
void srv_timer_cd_reset(void);

bool srv_timer_cd_is_running(void);

/** @brief 是否已启动过且有进度（运行中或暂停中） */
bool srv_timer_cd_is_active(void);

/** @brief 是否已经走到 0（提示音已响过） */
bool srv_timer_cd_is_finished(void);

/** @brief 剩余毫秒数（已到点为 0） */
int64_t srv_timer_cd_remaining_ms(void);

#ifdef __cplusplus
}
#endif
