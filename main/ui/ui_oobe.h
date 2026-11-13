#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 播放「方块翻成字母」开机动画（仿 ClassIsland OOBE 片头）
 *
 * 在当前屏幕最上层铺一层全屏黑底覆盖层，然后依次播放：
 *
 *   ① 整层入场（整体向上收一点，方块自己带淡入）
 *   ② 11 个方块按正弦错峰从下方浮起
 *   ③ 方块绕左边缘「翻牌」到 90°（scale_x → 0）
 *   ④ 同一时刻字母从 −90° 翻回正面（scale_x 0 → 1），接缝处内容交换
 *   ⑤ 字距收紧：格子宽度与间距一起归零，标题回到自然排版
 *   ⑥ 整层淡出，露出下面已经建好的页面
 *
 * 动画跑完覆盖层自己删掉，调用方不需要收尾；重复调用会先把上一轮清干净
 * （所以「关于」页可以拿它当重播按钮）。
 *
 * 必须在持有 LVGL 锁时调用 —— app_navigator_init() 的锁内、以及任何 LVGL
 * 事件回调里都满足这个条件。
 */
void ui_oobe_play(void);

/**
 * @brief 当前是否正在播放（淡出走完、覆盖层开始销毁时变 false）
 */
bool ui_oobe_is_playing(void);

#ifdef __cplusplus
}
#endif
