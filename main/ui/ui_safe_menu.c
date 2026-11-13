/**
 * @file ui_safe_menu.c
 * @brief 安全菜单：手势（连点 5 次 + 右滑）打开的入口页
 *
 * 左右两张大卡：
 *   左「功能菜单」  → PAGE_FUNC（书架 / 音乐 / 游戏 / 电台 / 照片 / 录音机）
 *   右「完整版设置」→ PAGE_SETTINGS_FULL（设置列表 + 安全模式开关）
 *
 * 为什么要有这个中间页：安全模式开着的时候，主菜单的「功能」卡和普通设置里的
 * 阅读/音乐/文件管理/快速返回主页都会被收起来；这个页是**唯一**能重新摸到
 * 它们的地方（也是唯一能关掉安全模式的地方），所以它自己不能带任何「安全」
 * 字样，标题就叫「菜单」。
 */
#include "ui_safe_menu.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "app_navigator.h"

#define CARD_W      ((ui_scr_w() - 16 - GAP) / 2)    /* 横屏 148，竖屏 108 */
#define CARD_H      ((ui_scr_h() >= 300) ? 216 : 168)
#define CARD_Y      34
#define GAP         8

static void on_func(lv_event_t *e)
{
    (void)e;
    app_navigator_switch(PAGE_FUNC, true);
}

static void on_settings(lv_event_t *e)
{
    (void)e;
    app_navigator_switch(PAGE_SETTINGS_FULL, true);
}

void ui_safe_menu_create(lv_obj_t *root)
{
    ui_titlebar_create(root, "菜单", app_navigator_back_cb, NULL);

    ui_card_create(root, 8, CARD_Y, CARD_W, CARD_H,
                   &ic_func, "功能菜单", 0x1585C0, on_func, NULL);
    ui_card_create(root, 8 + CARD_W + GAP, CARD_Y, CARD_W, CARD_H,
                   &ic_setting, "完整版设置", 0x606A78, on_settings, NULL);
}