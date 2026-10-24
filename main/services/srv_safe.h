#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 安全模式（隐蔽模式）
 *
 * 入口：时钟页**左上角连点 5 次，然后在 2 秒内右滑**；退出用同一套手势。
 * 开启后界面上跟阅读/音乐有关的东西全部收起来：
 *   - 主菜单的「功能」整张卡（书架 / 音乐 / 游戏 / 照片 / 录音机）
 *   - 设置里的「阅读」「音乐」「文件管理」
 *
 * 开关状态存在 /config.dat 的 safe_mode 里，**重启后保持**（要关掉得走
 * 手势进安全菜单 → 完整版设置 → 安全模式）。只有「连点/等右滑」这套手势
 * 状态是内存里的，重启自然清空。
 */
bool srv_safe_on(void);

/**
 * @brief 切换安全模式并打日志
 * @return 切换后的状态（true = 安全模式开着）
 */
bool srv_safe_toggle(void);

/**
 * @brief 左上角点一下（时钟页热区收到 CLICKED 时调）
 *
 * 连点够 5 次且都在 3 秒内 → 进入「等右滑确认」状态。
 */
void srv_safe_tap(void);

/*! 是否已经连点够 5 次、正在等右滑（超时自动失效） */
bool srv_safe_armed(void);

/*! 取消「等右滑」状态（手势用掉或超时后用） */
void srv_safe_disarm(void);

#ifdef __cplusplus
}
#endif