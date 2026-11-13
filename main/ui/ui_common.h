#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== 调色板 ===================== */
#define UI_COLOR_BG        lv_color_hex(0x000000)   /*!< 纯黑背景 */
#define UI_COLOR_SURFACE   lv_color_hex(0x161B22)   /*!< 卡片/面板底色 */
#define UI_COLOR_PRIMARY   lv_color_hex(0x1585C0)   /*!< 主题色 */
#define UI_COLOR_TEXT      lv_color_hex(0xE6E9EF)   /*!< 主文字 */
#define UI_COLOR_TEXT_DIM  lv_color_hex(0x8B949E)   /*!< 次要文字 */
#define UI_COLOR_TEMP      lv_color_hex(0xE5484D)   /*!< 温度红 */
#define UI_COLOR_HUMI      lv_color_hex(0x3B9EFF)   /*!< 湿度蓝 */
#define UI_COLOR_WARN      lv_color_hex(0xF5A623)   /*!< 警告黄 */
#define UI_COLOR_OK        lv_color_hex(0x2E8B57)   /*!< 正常绿 */

/* ===================== 布局常量（横屏 320x240） ===================== */
#define UI_SCREEN_W        320
#define UI_SCREEN_H        240
#define UI_TITLEBAR_H      28
#define UI_MARGIN          8

/*! 标题栏以下的内容起点：各菜单页第一行卡片的 y */
#define UI_CONTENT_Y       (UI_TITLEBAR_H + 6)

/*! 亮度的下限：0% 保留给休眠，界面上不允许调到 0 */
#define UI_BRIGHTNESS_MIN  1
#define UI_BRIGHTNESS_MAX  100

/*! 手势判定：横向位移阈值(px)，且横向位移需大于纵向的 2 倍 */
#define UI_SWIPE_MIN_PX    50

/* ===================== 设置列表行（各二级设置页共用） ===================== */
#define UI_ROW_X     8
/*! 行宽跟随当前逻辑屏宽：竖屏 240 下不能再用横屏的 304 */
#define UI_ROW_W     (ui_scr_w() - UI_ROW_X * 2)
#define UI_ROW_H     40      /*!< 行高必须放得下 32x32 图标 */
#define UI_ROW_GAP   4
#define UI_ROW_TOP   4       /*!< 列表顶部留白 */

/*! 第 idx 行的 y 坐标（相对列表容器） */
#define UI_ROW_Y(idx)   (UI_ROW_TOP + (lv_coord_t)(idx) * (UI_ROW_H + UI_ROW_GAP))

/* 取 LVGL 互斥锁的超时（ms）。
 * 注意 esp_lv_adapter 的语义：0 = 只试一次不等待，负数 = 永久等待。
 * 所以这里必须给正数超时，否则一旦撞上 LVGL 任务正在渲染就会抢锁失败，
 * 导致页面创建被整体跳过（表现为黑屏）。 */
#define UI_LOCK_TIMEOUT_MS   1000

/* 内置字体 */
extern const lv_font_t lv_font_montserrat_14;

/* 自定义字体 */
LV_FONT_DECLARE(font_ui_14);      /* 14px 中文 UI */
LV_FONT_DECLARE(font_ipa_14);     /* 14px 音标（IPA 字符，中文那套字体里没有） */
LV_FONT_DECLARE(font_clock_48);   /* 48px 时钟数字 */

/**
 * @brief 初始化全局深色主题（屏幕创建前调用）
 */
void ui_common_init(void);

/* ===================== 页面骨架 ===================== */

/**
 * @brief 创建页面根容器
 *
 * 结构是 screen -> root，所有页面控件都挂在 root 上，滑入动画也作用在 root。
 *
 * 为什么不直接对 screen 做动画、也不直接往 screen 上挂控件：
 * screen 的尺寸/坐标由 display 托管，给它设 LV_PCT 尺寸或 set_x 位移
 * 会引起重绘区域计算异常，表现为"旧界面文字残留在新界面上"。
 * root 是普通对象（显式 320x240、纯黑不透明、禁止滚动），不会有这个问题。
 *
 * @param parent 屏幕对象
 * @return 页面根容器
 */
lv_obj_t *ui_page_root_create(lv_obj_t *parent);

/**
 * @brief 页面入场动画
 * @param obj        页面根容器
 * @param from_right true = 从屏幕右侧滑入（进入下级页）；false = 从左侧滑入（返回上级页）
 */
void ui_slide_in(lv_obj_t *obj, bool from_right);

/*! 滑入方向 */
typedef enum {
    UI_SLIDE_FROM_LEFT = 0,     /*!< 从左往右飞入 */
    UI_SLIDE_FROM_RIGHT,        /*!< 从右往左飞入 */
    UI_SLIDE_FROM_BOTTOM,       /*!< 从下往上飞入（日历） */
} ui_slide_dir_t;

/**
 * @brief 指定方向的入场动画（ui_slide_in 是它的横向特例）
 */
void ui_slide_in_dir(lv_obj_t *obj, ui_slide_dir_t dir);

/**
 * @brief 给页面根容器开启手势
 *        时钟页左滑进主菜单；其它页右滑返回上级。
 *        按在滑块上起手时不算手势（避免拖亮度条误触返回）。
 */
void ui_enable_swipe(lv_obj_t *root);

/* ===================== 通用组件 ===================== */

/**
 * @brief 当前 LVGL 逻辑屏幕宽高（会随运行时旋转变化）
 */
lv_coord_t ui_scr_w(void);
lv_coord_t ui_scr_h(void);

/**
 * @brief 在标题栏右侧放一个工具按钮（搜索、旋转等）
 * @param right_ofs 距标题栏右侧偏移，例如 -8
 * @return 按钮对象
 */
lv_obj_t *ui_titlebar_button_create(lv_obj_t *bar, const char *text,
                                    lv_coord_t right_ofs, lv_coord_t w,
                                    lv_event_cb_t cb, void *arg);

/**
 * @brief 在标题栏返回键右侧放一个工具按钮（旋转等）
 * @param left_ofs 距标题栏左侧偏移，例如 32
 */
lv_obj_t *ui_titlebar_left_button_create(lv_obj_t *bar, const char *text,
                                         lv_coord_t left_ofs, lv_coord_t w,
                                         lv_event_cb_t cb, void *arg);

/**
 * @brief 统一标题栏：标题居中、左侧返回键、右侧 HH:MM
 * @param back_cb 传 NULL 则不显示左侧返回键（主菜单那种手势返回的页面）
 * @return 标题栏对象；后续可用 ui_titlebar_set_title() 改标题
 */
lv_obj_t *ui_titlebar_create(lv_obj_t *parent, const char *title,
                             lv_event_cb_t back_cb, void *back_arg);

/**
 * @brief 同 ui_titlebar_create，但右侧不显示 HH:MM
 *
 * 游戏页右侧要放自己的菜单键，标题限宽也相应放宽（左右各让开一个按钮位）。
 * 返回值同样可以用 ui_titlebar_set_title() 改标题。
 */
lv_obj_t *ui_titlebar_create_plain(lv_obj_t *parent, const char *title,
                                   lv_event_cb_t back_cb, void *back_arg);

/**
 * @brief 改标题栏上的标题文字
 *
 * 给「标题要晚一步才知道」的页面用（阅读器打开完才拿得到书名）。
 * 传进来的必须是 ui_titlebar_create() 返回的那个对象。
 */
void ui_titlebar_set_title(lv_obj_t *bar, const char *title);

/**
 * @brief 开关标题的循环滚动（默认开）
 *
 * 阅读器和游戏页关掉：标题常驻视野里，一直滚会分心，
 * 关掉后长标题按省略号收尾。
 */
void ui_titlebar_set_scroll(lv_obj_t *bar, bool on);

/**
 * @brief 弹出音量调节层（半透明遮罩 + 滑条，点空白处关闭）
 *
 * 挂在当前屏幕上，切页时随屏幕一起销毁；同一时刻只会有一个。
 * 调用前需已持有 LVGL 锁（普通页面回调里本来就在锁内）。
 */
void ui_volume_popup_show(void);

/**
 * @brief 卡片：圆角面板 + 图标 + 文字，带按下反馈
 */
lv_obj_t *ui_card_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                         lv_coord_t w, lv_coord_t h,
                         const lv_image_dsc_t *icon, const char *label,
                         uint32_t color, lv_event_cb_t cb, void *arg);

/**
 * @brief 给任意可点击对象加统一的按下反馈（背景变亮）
 */
void ui_add_click_feedback(lv_obj_t *obj);

/**
 * @brief 构建"待实现"占位内容（不创建屏幕）
 */
void ui_placeholder_build(lv_obj_t *root, const char *title, const char *note,
                          lv_event_cb_t back_cb, void *back_arg);

/* ===================== 设置页共用组件 ===================== */

/**
 * @brief 创建可纵向滚动的设置列表容器（铺满标题栏以下）
 */
lv_obj_t *ui_settings_list_create(lv_obj_t *root);

/**
 * @brief 创建一行设置项：圆角面板 + 可选图标 + 名称
 *
 * 只负责左侧部分，右侧控件（按钮/数值/开关）由调用方用 ui_row_btn_create()
 * 或自己往里加 —— 各页右侧组合差别太大，硬做成一种模版反而处处别扭。
 *
 * @param y 行在列表里的 y（用 UI_ROW_Y(idx)）
 * @return 行对象
 */
lv_obj_t *ui_row_create(lv_obj_t *parent, lv_coord_t y,
                        const lv_image_dsc_t *icon, const char *name);

/**
 * @brief 在行右侧创建一个小按钮（内嵌深色底 + 居中文字）
 * @param right_ofs 距行右边的偏移（负值，如 -8 表示贴右）
 * @param cb        为 NULL 时不可点击（纯展示）
 * @return 按钮上的文字标签，后续改字用
 */
lv_obj_t *ui_row_btn_create(lv_obj_t *row, lv_coord_t right_ofs, lv_coord_t w,
                            const char *text, lv_event_cb_t cb, void *arg);

/**
 * @brief 在行右侧创建一个纯文字数值（无底色，居中在固定宽度里）
 * @return 文字标签
 */
lv_obj_t *ui_row_value_create(lv_obj_t *row, lv_coord_t right_ofs, lv_coord_t w);

/**
 * @brief 在行右侧创建一个钮子开关（可点、可拖）
 *
 * 点击和拖动都会发 LV_EVENT_VALUE_CHANGED；回调里只管存配置，
 * 存完调 refresh 会再同步一次界面，结果一致。
 *
 * @param row 行对象（ui_row_create 的返回值）
 * @param on  初始状态
 * @param cb  状态变化回调，可为 NULL
 * @return 开关对象，用 ui_row_switch_set() 刷新
 */
lv_obj_t *ui_row_switch_create(lv_obj_t *row, bool on, lv_event_cb_t cb, void *arg);

/**
 * @brief 刷新开关的状态（程序改状态不会触发回调）
 */
void ui_row_switch_set(lv_obj_t *sw, bool on);

/* ===================== 状态栏图标（代码绘制） ===================== */

/**
 * @brief 代码绘制电池图标 + 百分比数字
 *
 * 组成：外壳(圆角矩形描边) + 正极凸点 + 内部电量填充条 + 右侧百分比。
 * 低电量（<=20%）填充变红。
 *
 * @return 图标根容器；后续用 ui_battery_set() 更新
 */
lv_obj_t *ui_battery_create(lv_obj_t *parent, lv_align_t align,
                            lv_coord_t x_ofs, lv_coord_t y_ofs);

/**
 * @brief 更新电池显示
 * @param pct   电量 0-100
 * @param valid false 时整个图标隐藏
 */
void ui_battery_set(int pct, bool valid);

/**
 * @brief 创建 SD 卡状态图标（使用 ic_SD，默认隐藏）
 */
lv_obj_t *ui_sd_icon_create(lv_obj_t *parent, lv_align_t align,
                            lv_coord_t x_ofs, lv_coord_t y_ofs);

/**
 * @brief 有卡才显示图标，没卡直接不显示
 */
void ui_sd_icon_set(bool mounted);

#ifdef __cplusplus
}
#endif
