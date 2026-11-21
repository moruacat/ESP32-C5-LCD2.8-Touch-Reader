#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 阅读器正文字体（多档字号）
 *
 * 14px 编在固件里；16/18/20/22px 用 lv_font_conv 生成的二进制字库。
 * 查找顺序：SD 卡 `/fonts/font_ui_<px>.bin` → Flash 的 `font` SPIFFS 分区
 * （构建时由 tools/gen_read_fonts.py 生成并打包，没插 SD 也能用）。
 *
 * 加载失败（文件不在/损坏）自动回退 14px，并在串口打一行原因。
 * 加载成功的动态字体在释放前一直缓存，避免每次进阅读器都读 2MB。
 */

/** 取指定字号的字体；没有对应字库时返回 14px（不返回 NULL） */
lv_font_t *ui_read_font_get(int px);

/** 该字号是否可用（14 恒真；其余检查 SD /fonts 与 Flash 字库分区） */
bool ui_read_font_available(int px);

/** 下一个可用字号（14 → 16 → 18 → 20 → 22 → 14） */
int ui_read_font_next(int px);

/** 上一个可用字号（14 → 22 → 20 → 18 → 16 → 14） */
int ui_read_font_prev(int px);

/**
 * @brief 释放动态加载的字库（回退到 14px）
 *
 * 只在真正离开阅读器后调用：转屏重建时旧页面 delete 得晚于新页面 build，
 * 那时释放会把新页面正在用的字体指针变成野指针。
 */
void ui_read_font_release(void);

#ifdef __cplusplus
}
#endif
