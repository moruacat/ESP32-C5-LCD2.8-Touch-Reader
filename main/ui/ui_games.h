#pragma once
#include "lvgl.h"
#include "app_navigator.h"

#ifdef __cplusplus
extern "C" {
#endif

/*! 「重新开始」的回调（各游戏自己实现） */
typedef void (*ui_game_restart_cb_t)(void);

/*! 滑动手势方向回调：0=上 1=下 2=左 3=右 */
typedef void (*ui_game_swipe_cb_t)(int dir, void *arg);

/**
 * @brief 游戏列表页（功能菜单 -> 游戏）
 */
void ui_games_create(lv_obj_t *root);

/**
 * @brief 进入游戏页的统一动作（每个游戏在 create 开头调一次）
 *
 * 记住本页的根容器 / 页面 id / 重新开始回调，并停掉后台音乐 ——
 * 后台播放是「点后台播放后退出播放器界面」，用户完全可能带着音乐一路点到
 * 游戏，解码任务会和游戏抢 CPU 与内存，这里主动收掉。
 *
 * @param root       页面根容器
 * @param page       本页的页面 id（守卫用）
 * @param on_restart 菜单/弹层里「重新开始」要调的函数
 */
void ui_game_page_enter(lv_obj_t *root, app_page_id_t page,
                        ui_game_restart_cb_t on_restart);

/**
 * @brief 当前是否还停在本游戏页上
 *
 * 页面是异步删除的，菜单与弹层的回调可能晚于切页送达 ——
 * 各游戏的重置动作执行前先问一句，避免改到已经切走的页面。
 */
bool ui_game_page_active(void);

/**
 * @brief 给 ui_alert_show 用的「再来一局」回调：内部转调本页的 restart
 */
void ui_game_alert_restart_cb(void *arg);

/**
 * @brief 游戏页公共标题栏：左返回键 + 居中标题 + 右侧菜单键
 *
 * 骨架复用 ui_titlebar_create_plain（右侧不放时钟），菜单里有
 * 「重新开始 / 退出游戏」两项。
 *
 * @param title 游戏名
 * @return 标题栏对象，游戏可以往上面挂分数之类的信息
 */
lv_obj_t *ui_game_bar_create(lv_obj_t *root, const char *title);

/**
 * @brief 给页面根容器装四方向滑动手势：按下记起点，松手按位移判方向
 *
 * 位移不足 18px（轻点）不回调。
 * 注意棋盘、格子这类子对象要摘掉 LV_OBJ_FLAG_CLICKABLE：LVGL 的命中测试
 * 会把按下判给最上层的可点击对象且不再往上传，不摘的话 root 收不到事件。
 */
void ui_game_swipe_enable(lv_obj_t *root, ui_game_swipe_cb_t cb, void *arg);

/**
 * @brief 把定时器的生命周期绑到页面根容器上（页面销毁时自动删除）
 *
 * 句柄跟着事件 user_data 走，不用 static 存 —— 否则「游戏 A → 别的 → A」
 * 时新页面已经把 static 覆盖，旧页面销毁时删掉的就是新页面的定时器。
 */
void ui_game_timer_bind(lv_obj_t *root, lv_timer_t *tmr);

#ifdef __cplusplus
}
#endif