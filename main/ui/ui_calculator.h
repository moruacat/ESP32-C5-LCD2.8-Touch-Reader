#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 计算器页面（功能菜单 -> 计算器）
 *
 * 四则运算 + 百分号 + 正负号 + 退格，运算按输入顺序从左到右，
 * 不做优先级（普通计算器的行为）。
 */
void ui_calculator_create(lv_obj_t *root);

#ifdef __cplusplus
}
#endif
