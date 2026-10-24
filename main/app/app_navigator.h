#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 页面 ID（路由表）
 */
typedef enum {
    PAGE_CLOCK = 0,     /*!< 时钟伪装层（主页） */
    PAGE_HOME,          /*!< 主菜单（卡片） */
    PAGE_FUNC,          /*!< 功能菜单（书架/音乐/游戏/录音机） */
    PAGE_SETTINGS,      /*!< 设置（列表） */
    PAGE_SET_NET,       /*!< 设置->网络 */
    PAGE_SET_NET_ADD,   /*!< 设置->网络->添加网络 */
    PAGE_SET_TIME,      /*!< 设置->时间 */
    PAGE_SET_ALARM,     /*!< 设置->闹钟 */
    PAGE_SET_READ,      /*!< 设置->阅读 */
    PAGE_SET_MUSIC,     /*!< 设置->音乐 */
    PAGE_SET_BRIGHT,    /*!< 设置->亮度 */
    PAGE_SET_ABOUT,     /*!< 设置->关于 */
    PAGE_SET_BATTERY,   /*!< 设置->电池 */
    PAGE_FILES,         /*!< 文件管理（浏览 SD 卡） */
    PAGE_CALENDAR,      /*!< 日历（时钟页上滑） */
    PAGE_SAFE_MENU,     /*!< 安全菜单（时钟页手势：连点 5 次 + 右滑） */
    PAGE_SETTINGS_FULL, /*!< 完整版设置（安全菜单右侧入口，含安全模式开关） */
    PAGE_TIMER,         /*!< 计时器 */
    PAGE_COUNTDOWN,     /*!< 倒计时 */
    PAGE_BOOKSHELF,     /*!< 书架 */
    PAGE_READER,        /*!< 阅读器 */
    PAGE_MUSIC,         /*!< 音乐 */
    PAGE_GAMES,         /*!< 游戏列表 */
    PAGE_GAME_2048,     /*!< 游戏：2048 */
    PAGE_GAME_SNAKE,    /*!< 游戏：贪吃蛇 */
    PAGE_GAME_GOMOKU,   /*!< 游戏：五子棋 */
    PAGE_GAME_PAINT,    /*!< 游戏：画板 */
    PAGE_GAME_BALL,     /*!< 游戏：重力球 */
    PAGE_GAME_SOCCER,   /*!< 游戏：足球 */
    PAGE_RECORDER,      /*!< 录音机 */
    PAGE_PHOTOS,        /*!< 照片（列表 + 全屏横滑浏览） */
    PAGE_WORDS,         /*!< 单词（查词） */
    PAGE_RADIO,         /*!< 电台（中国之声 · HLS 在线直播） */
    PAGE_MAX,
} app_page_id_t;

/**
 * @brief 每个页面的构建回调
 * @param root 页面根容器（导航器已建好，页面把控件挂到它上面即可）
 */
typedef struct {
    void (*build)(lv_obj_t *root);
} app_page_ops_t;

/**
 * @brief 初始化导航器：安装主题并进入时钟页
 */
void app_navigator_init(void);

/*! 切页动画方向（给「不是普通下级页」的入口用） */
typedef enum {
    APP_NAV_ANIM_AUTO = 0,  /*!< 按 push/back 决定：进下级从右入、返回从左入 */
    APP_NAV_ANIM_LEFT,      /*!< 从左往右飞入（安全菜单） */
    APP_NAV_ANIM_BOTTOM,    /*!< 从下往上飞入（日历） */
} app_nav_anim_t;

/**
 * @brief 切换页面并指定入场动画
 *
 * 用法与 app_navigator_switch 相同，只是多一个方向；日历（从下往上）和
 * 安全菜单（从左往右）用它，其余页面用默认那个就够。
 *
 * @param anim 见 app_nav_anim_t
 */
void app_navigator_switch_anim(app_page_id_t page, bool push, app_nav_anim_t anim);

/**
 * @brief 切换到指定页面
 * @param page 目标页面
 * @param push true = 压栈并「从右滑入」；false = 清空返回栈并「从左侧滑入」
 */
void app_navigator_switch(app_page_id_t page, bool push);

/**
 * @brief 返回上一页（栈空则回时钟页），从左侧滑入
 */
void app_navigator_back(void);

/**
 * @brief 快速隐藏：清空栈并回到时钟页
 */
void app_navigator_hide(void);

/**
 * @brief 当前页面 ID
 */
app_page_id_t app_navigator_current(void);

/**
 * @brief 通用返回回调：直接挂到标题栏返回键上即可
 *        用法：ui_titlebar_create(root, "设置", app_navigator_back_cb, NULL);
 */
void app_navigator_back_cb(lv_event_t *e);

/**
 * @brief 智能返回回调：短按逐级返回、长按直达主菜单
 *
 * 设置里打开「快速返回主页」后两者互换（短按直达主菜单、长按逐级返回）。
 * 只给「功能菜单」那一路的页面用；阅读器仍然用 app_navigator_back_cb，
 * 退出阅读固定逐级返回。
 *
 * 注意它同时吃 CLICKED 和 LONG_PRESSED 两种事件。
 */
void app_navigator_back_smart_cb(lv_event_t *e);

/**
 * @brief 直达主菜单：清空返回栈并切到主菜单
 */
void app_navigator_home(void);

/**
 * @brief 压入一个「待实现」占位页（用于尚未做的二级菜单）
 */
void app_navigator_push_placeholder(const char *title, const char *note);

#ifdef __cplusplus
}
#endif
