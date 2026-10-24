#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 注册闹钟响铃提示
 *
 * 在 srv_alarm_init() 与 srv_input_init() 之后调用一次。
 *
 * 响铃时会**在当前页面上**弹一层提示（无论用户在看哪一页），
 * 两种停法都走 srv_alarm_stop()：
 *   - 点提示上的「停止」
 *   - 按 BOOT 键（任意一种短按/双击/长按都算）
 */
void ui_alarm_alert_init(void);

#ifdef __cplusplus
}
#endif
