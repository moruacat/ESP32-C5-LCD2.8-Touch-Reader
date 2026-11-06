#pragma once
#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== 确认对话框 ===================== */

typedef void (*ui_dialog_ok_cb_t)(void *arg);

/**
 * @brief 二次确认对话框（危险操作用）
 *
 * 标题用红色，并且**突出「取消」按钮**（主色实心）——确定按钮反而做得低调，
 * 避免误触把设置清掉。点确定后先关掉面板再回调，回调里可以放心重建页面内容。
 *
 * @param title   标题（红字）
 * @param note    说明文字，可为 NULL
 * @param ok_text 确定按钮文字（如"清空"），为 NULL 用"确定"
 */
void ui_dialog_confirm(const char *title, const char *note, const char *ok_text,
                       ui_dialog_ok_cb_t ok_cb, void *arg);

/* ===================== 提示层（只有一个按钮） ===================== */

typedef void (*ui_alert_cb_t)(void *arg);

/**
 * @brief 弹一个提示层：标题 + 说明 + 一个按钮
 *
 * 挂在当前活动屏幕上，所以无论用户在哪一页都能看到；切换页面时随旧屏幕销毁。
 *
 * @param title_color    标题颜色（倒计时用警告黄、闹钟用红）
 * @param note           说明文字，可为 NULL
 * @param btn_text       按钮文字
 * @param cb             点按钮时的回调（可为 NULL，纯关闭）
 * @param auto_close_ms  >0 时到点自动关闭；0 表示不自动关（由调用方控制）
 */
void ui_alert_show(const char *title, lv_color_t title_color, const char *note,
                   const char *btn_text, ui_alert_cb_t cb, void *arg,
                   uint32_t auto_close_ms);

/**
 * @brief 关掉当前屏幕上的提示层（没有则什么都不做）
 */
void ui_alert_close(void);

/**
 * @brief 提示层是否还显示着
 *
 * 给「闹钟还在响但提示层被切页带走了」这种情况用：调用方可以据此重新弹一次，
 * 而不是每秒无脑重建（那样会闪）。
 */
bool ui_alert_visible(void);

/* ===================== 多列滚轮选择器 ===================== */

/**
 * @brief 一列滚轮的定义
 */
typedef struct {
    const char *label;   /*!< 列下方的单位文字，可为 NULL */
    int         start;   /*!< 起始值（可负） */
    int         count;   /*!< 项数，不能为 0 */
    int         value;   /*!< 当前值，需落在 [start, start+count-1]，超出会被夹住 */
} ui_picker_col_t;

/*! 选择结果：values[i] = 第 i 列的实际数值（已加回 start） */
typedef void (*ui_picker_done_cb_t)(const int *values, void *arg);

/**
 * @brief 弹出多列滚轮选择器（时间、日期、时段等用）
 * @param col_count 列数，1~6
 * @param cols      列定义数组
 */
void ui_picker_show(const char *title, int col_count, const ui_picker_col_t *cols,
                    ui_picker_done_cb_t cb, void *arg);

/* ===================== 文字输入层（标题 + 输入框 + 屏幕键盘） ===================== */

/*! 输入结果回调：text 是输入框里的内容（可能是空串） */
typedef void (*ui_text_input_cb_t)(const char *text, void *arg);

/**
 * @brief 弹出文字输入层（WiFi 密码这类需要打字的地方用）
 *
 * 顶部是标题 + 取消 + 输入框，下方是屏幕键盘，整层挂在当前活动屏幕上
 * （切页随屏幕一起销毁）。按键盘上的确定键回调 cb，按取消键或左上角
 * 「取消」则只关闭不回调。
 *
 * @param title   标题（超过宽度会自动省略号）
 * @param max_len 最多输入多少字符，0 或超限则按内部上限截断
 */
void ui_text_input_show(const char *title, uint32_t max_len,
                        ui_text_input_cb_t cb, void *arg);

/**
 * @brief 同上，但输入框预填 initial（再次点开时保留上次内容，方便改）
 *
 * @param initial 预填内容，可为 NULL；末尾自动放光标
 */
void ui_text_input_show_init(const char *title, uint32_t max_len, const char *initial,
                             ui_text_input_cb_t cb, void *arg);

/* ===================== 数字密码输入层（纯数字键盘） ===================== */

/**
 * @brief 弹出 6 位数字密码输入层（照片密码用）
 *
 * 与 ui_text_input_show 的区别：只有 0-9 三个一行的数字键 + 退格 + 确定，
 * 输入内容用一排圆点显示（不回显明文），输满 6 位自动提交。
 *
 * @param title 标题（如「输入照片密码」）
 * @param cb    输满 6 位或按确定时回调，text 就是这 6 位数字；
 *              按取消 / 点空白关闭则不回调
 */
void ui_pass_input_show(const char *title, ui_text_input_cb_t cb, void *arg);

/* ===================== 操作菜单 ===================== */

/*! 菜单最多几项（不含自动追加的「取消」） */
#define UI_MENU_MAX_ITEMS  5

/**
 * @brief 菜单项
 */
typedef struct {
    const char           *text;   /*!< 项文字 */
    const lv_image_dsc_t *icon;   /*!< 左侧图标，可为 NULL（此时文字居中） */
} ui_menu_item_t;

/*! 菜单结果回调：idx = 选中项序号（0 起）；-1 = 点了「取消」或空白处 */
typedef void (*ui_menu_cb_t)(int idx, void *arg);

/**
 * @brief 弹出操作菜单（长按列表项时用）
 *
 * 面板居中：顶部是标题（书名这类上下文），中间按顺序列出各项，
 * 末尾自动追加一项「取消」。点空白处等同取消。
 * 回调前会先把菜单关掉，回调里可以放心重建页面内容。
 *
 * @param title 菜单标题，可为 NULL
 * @param items 项数组，至少 1 项、最多 UI_MENU_MAX_ITEMS 项
 */
void ui_menu_show(const char *title, const ui_menu_item_t *items, int count,
                  ui_menu_cb_t cb, void *arg);

/* ===================== 长文本层（简介等） ===================== */

/**
 * @brief 弹一个可滚动的纯文本层：标题 + 正文 + 关闭
 *
 * 面板固定尺寸，正文用 14px 自动换行，超出高度可以上下滑。
 *
 * @param title 标题（一般是书名），可为 NULL
 * @param text  正文，可为 NULL（显示"（无内容）"）
 */
void ui_info_show(const char *title, const char *text);

/* ===================== 封面层 ===================== */

/**
 * @brief 弹一个封面层：铺满整屏，点任意位置关闭
 *
 * 封面数据是 RGB565 原始像素（EBF 的 cover.raw）。面板物理是竖的
 * 240x320，而 UI 按横屏排，所以这里把封面转 90° 铺满 —— 看的时候把
 * 设备竖起来，封面才是正的。转完仍按比例缩放，不会变形。
 *
 * **本函数接管 data 的所有权**：弹层关闭（或本函数提前返回）时由它 free()，
 * 调用方不要再释放。
 *
 * @param data  RGB565 像素缓冲，可为 NULL（此时什么都不弹，直接返回）
 * @param bytes data 字节数
 * @param w     宽（像素）
 * @param h     高（像素）
 */
void ui_cover_show(uint8_t *data, size_t bytes, int32_t w, int32_t h);

#ifdef __cplusplus
}
#endif
