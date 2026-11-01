#pragma once
#include "ui_dialog.h"      /* ui_text_input_cb_t：与英文键盘那一层共用同一个回调类型 */

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== 中文输入层（拼音键盘） ===================== */

/**
 * @brief 弹出中文输入层（标题 + 取消 + 输入框 + 拼音键盘）
 *
 * 用法与 ui_text_input_show() 完全一致，调用方不必区分两者：顶部同样是
 * 标题 + 取消 + 输入框，下方是键盘，按键盘上的确定键回调 cb，按取消键或
 * 左上角「取消」则只关闭不回调。区别只在键盘 —— 这里用 components/lvgl_pinyin_ime
 * 的拼音键盘，能直接把汉字提交进输入框。
 *
 * 两个退化情形（都不需要调用方处理）：
 *   - 构建里没有编入拼音组件（模拟器），自动转调 ui_text_input_show()；
 *   - 真机上拼音引擎/字库没就绪（dict、font 分区还没烧），同上退化成英文键盘。
 *
 * @param title   标题（超过宽度会自动省略号）
 * @param max_len 最多输入多少字符，0 或超限则按内部上限截断
 * @param cb      输入结果回调，可为 NULL
 * @param arg     回调透传参数
 */
void ui_ime_text_input_show(const char *title, uint32_t max_len,
                            ui_text_input_cb_t cb, void *arg);

/**
 * @brief 开机自检：把「输入法为什么没出来」提前打到日志上
 *
 * 只查 SD 卡上的词库/字库在不在、当前最大连续内存够不够，**不加载字体**
 * （字体真加载要几 MB 连续内存，不能为了自检先白占着；真到第一次点开输入法时
 * 组件自己会按同一套规则挑档位，并打出字号/字节数那几行日志）。
 *
 * 在 SD 卡挂载之后、UI 起来之前调用一次即可；真正的输入能力不受它影响。
 */
void ui_ime_diag_log(void);

#ifdef __cplusplus
}
#endif