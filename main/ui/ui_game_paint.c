#include "ui_game_paint.h"
#include "ui_common.h"
#include "ui_games.h"
#include "ui_dialog.h"
#include "app_navigator.h"

#include <stdint.h>
#include <stdlib.h>

/* 画布区：标题栏以下、工具条以上 */
#define AREA_Y      UI_TITLEBAR_H          /* 28 */
#define AREA_H      172                    /* 28..200 */

/* 工具条 */
#define BAR_Y       200
#define BAR_H       40
#define SWATCH      22
#define SWATCH_Y    (BAR_Y + 9)

/*! 一笔最多能记多少点（超过就按上限不再增长） */
#define STROKE_MAX  4096

static const uint32_t PALETTE[] = {
    0xE6E9EF,      /* 白 */
    0xE5484D,      /* 红 */
    0x3BD07A,      /* 绿 */
    0x3B9EFF,      /* 蓝 */
    0xF5A623,      /* 橙 */
};
#define PAL_N   ((int)(sizeof(PALETTE) / sizeof(PALETTE[0])))

static const int WIDTHS[] = {1, 3, 6};
#define W_N     ((int)(sizeof(WIDTHS) / sizeof(WIDTHS[0])))

static lv_obj_t *s_root;
static lv_obj_t *s_area;

static lv_obj_t *s_color_btn[PAL_N];
static lv_obj_t *s_width_btn[W_N];

static lv_color_t s_color;
static int        s_color_idx;
static int        s_width;

/* 当前这一笔 */
static lv_obj_t                 *s_cur;
static lv_point_precise_t       *s_pts;
static uint32_t                  s_cnt;
static uint32_t                  s_cap;
static bool                      s_drawing;

/* ===================== 笔画数据 ===================== */

/**
 * @brief 笔画点数组跟着 line 对象一起销毁
 *
 * 数组必须活到 line 被删为止（lv_line_set_points 只存指针不拷贝），
 * 而 line 可能因为切页、清空、重开而消失 —— 把 free 挂在这里就不用
 * 再维护一张"谁还需要释放"的账，切页时也不会漏掉或重复释放。
 */
static void stroke_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    void *p = lv_obj_get_user_data(lv_event_get_target(e));
    if (p != NULL) {
        free(p);
    }
}

static void stroke_push(int x, int y)
{
    if (s_cur == NULL || s_pts == NULL) {
        return;
    }

    /* 前两个点强制记下来（起点要凑够两个点才画得出来），之后的点
     * 2px 以内的不记，省内存也省重绘 */
    if (s_cnt >= 2) {
        int dx = x - (int)s_pts[s_cnt - 1].x;
        int dy = y - (int)s_pts[s_cnt - 1].y;
        if (dx * dx + dy * dy < 4) {
            return;
        }
    }

    if (s_cnt >= STROKE_MAX) {
        return;
    }

    if (s_cnt >= s_cap) {
        uint32_t cap = (s_cap == 0) ? 64 : s_cap * 2;
        if (cap > STROKE_MAX) {
            cap = STROKE_MAX;
        }
        lv_point_precise_t *np =
            (lv_point_precise_t *)realloc(s_pts, sizeof(lv_point_precise_t) * cap);
        if (np == NULL) {
            return;
        }
        s_pts = np;
        s_cap = cap;
        lv_obj_set_user_data(s_cur, s_pts);     /* 地址变了，释放时要认新地址 */
    }

    s_pts[s_cnt].x = x;
    s_pts[s_cnt].y = y;
    s_cnt++;

    lv_line_set_points(s_cur, s_pts, s_cnt);
}

/* ===================== 工具 ===================== */

static void update_tool_ui(void)
{
    for (int i = 0; i < PAL_N; i++) {
        lv_obj_set_style_border_width(s_color_btn[i], (i == s_color_idx) ? 2 : 0, 0);
    }
    for (int i = 0; i < W_N; i++) {
        lv_obj_set_style_border_width(s_width_btn[i], (WIDTHS[i] == s_width) ? 2 : 0, 0);
    }
}

static void on_color(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= PAL_N) {
        return;
    }
    s_color_idx = idx;
    s_color     = lv_color_hex(PALETTE[idx]);
    update_tool_ui();
}

static void on_width(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= W_N) {
        return;
    }
    s_width = WIDTHS[idx];
    update_tool_ui();
}

static void clear_all(void)
{
    if (s_area == NULL) {
        return;
    }
    /* 从后往前删：删除会让后面的索引前移 */
    for (int i = (int)lv_obj_get_child_count(s_area) - 1; i >= 0; i--) {
        lv_obj_delete(lv_obj_get_child(s_area, (uint32_t)i));
    }
    s_cur     = NULL;
    s_pts     = NULL;
    s_cnt     = 0;
    s_cap     = 0;
    s_drawing = false;
}

static void on_clear(lv_event_t *e)
{
    (void)e;
    clear_all();
}

/* 菜单里的「重新开始」对画板就是清空 */
static void restart_game(void)
{
    if (!ui_game_page_active()) {
        return;
    }
    clear_all();
}

/* ===================== 绘制 ===================== */

static void on_area_pressed(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL || s_area == NULL) {
        return;
    }

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    int lx = p.x;
    int ly = p.y - AREA_Y;
    if (lx < 0 || lx >= UI_SCREEN_W || ly < 0 || ly >= AREA_H) {
        return;
    }

    s_pts = (lv_point_precise_t *)malloc(sizeof(lv_point_precise_t) * 64);
    if (s_pts == NULL) {
        return;
    }
    s_cnt = 0;
    s_cap = 64;

    s_cur = lv_line_create(s_area);
    lv_obj_remove_flag(s_cur, LV_OBJ_FLAG_CLICKABLE);   /* 别挡住后续触摸 */
    lv_obj_set_pos(s_cur, 0, 0);
    lv_obj_set_style_line_width(s_cur, s_width, 0);
    lv_obj_set_style_line_color(s_cur, s_color, 0);
    lv_obj_set_style_line_rounded(s_cur, true, 0);
    lv_obj_set_user_data(s_cur, s_pts);
    lv_obj_add_event_cb(s_cur, stroke_del_cb, LV_EVENT_DELETE, NULL);

    s_drawing = true;

    /* 起点要凑够两个点才画得出来（line 是连点成线），
     * 所以紧挨着再补一个点，配合圆头笔帽就是一个圆点 */
    stroke_push(lx, ly);
    stroke_push(lx + 1, ly);
}

static void on_area_pressing(lv_event_t *e)
{
    (void)e;
    if (!s_drawing) {
        return;
    }

    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    int lx = p.x;
    int ly = p.y - AREA_Y;
    if (lx < 0) lx = 0;
    if (lx >= UI_SCREEN_W) lx = UI_SCREEN_W - 1;
    if (ly < 0) ly = 0;
    if (ly >= AREA_H) ly = AREA_H - 1;

    stroke_push(lx, ly);
}

static void on_area_released(lv_event_t *e)
{
    (void)e;
    s_drawing = false;
    s_cur     = NULL;
    s_pts     = NULL;
    s_cnt     = 0;
    s_cap     = 0;
}

/* ===================== 页面 ===================== */

static void paint_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    /* 笔画数组由每个 line 自己的 DELETE 回调释放，这里只管把 static 指针清掉。
     * 切页是异步删除，手快「画板→别的→画板」时新页面已经建好，只有目标
     * 就是当前这份根对象时才清空。 */
    if (lv_event_get_target(e) != s_root) {
        return;
    }
    s_root = NULL;
    s_area = NULL;
    s_cur  = NULL;
    s_pts  = NULL;
    s_cnt  = 0;
    s_cap  = 0;
    for (int i = 0; i < PAL_N; i++) {
        s_color_btn[i] = NULL;
    }
    for (int i = 0; i < W_N; i++) {
        s_width_btn[i] = NULL;
    }
}

/* 工具条上的小按钮：圆角底 + 居中内容 */
static lv_obj_t *tool_btn(lv_obj_t *root, lv_coord_t x, lv_coord_t w,
                          lv_event_cb_t cb, void *arg)
{
    lv_obj_t *btn = lv_obj_create(root);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(btn, w, SWATCH);
    lv_obj_set_pos(btn, x, SWATCH_Y);
    lv_obj_set_style_bg_color(btn, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_border_color(btn, UI_COLOR_TEXT, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    ui_add_click_feedback(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, arg);
    return btn;
}

void ui_game_paint_create(lv_obj_t *root)
{
    s_root = root;
    ui_game_page_enter(root, PAGE_GAME_PAINT, restart_game);

    s_color     = lv_color_hex(PALETTE[0]);
    s_color_idx = 0;
    s_width     = WIDTHS[1];
    s_cur   = NULL;
    s_pts   = NULL;
    s_cnt   = 0;
    s_cap   = 0;
    s_drawing = false;

    ui_game_bar_create(root, "画板");

    s_area = lv_obj_create(root);
    lv_obj_remove_flag(s_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_area, UI_SCREEN_W, AREA_H);
    lv_obj_set_pos(s_area, 0, AREA_Y);
    lv_obj_set_style_bg_color(s_area, lv_color_hex(0x10151A), 0);
    lv_obj_set_style_bg_opa(s_area, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_area, 0, 0);
    lv_obj_set_style_radius(s_area, 0, 0);
    lv_obj_set_style_pad_all(s_area, 0, 0);
    lv_obj_add_flag(s_area, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(s_area, on_area_pressed,  LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(s_area, on_area_pressing, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_area, on_area_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_area, on_area_released, LV_EVENT_PRESS_LOST, NULL);

    /* ===== 工具条：5 个颜色 + 3 档粗细 + 清空 ===== */
    lv_obj_t *tool = lv_obj_create(root);
    lv_obj_remove_flag(tool, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(tool, UI_SCREEN_W, BAR_H);
    lv_obj_set_pos(tool, 0, BAR_Y);
    lv_obj_set_style_bg_color(tool, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(tool, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tool, 0, 0);
    lv_obj_set_style_radius(tool, 0, 0);
    lv_obj_set_style_pad_all(tool, 0, 0);

    for (int i = 0; i < PAL_N; i++) {
        lv_obj_t *btn = tool_btn(tool, 6 + i * 28, SWATCH, on_color,
                                 (void *)(intptr_t)i);
        s_color_btn[i] = btn;

        /* 圆点必须摘掉 CLICKABLE：lv_obj_create() 默认带这个标志，而命中测试
         * 会把按下事件判给最深层的可点击对象且不再往上传 —— 不摘的话点在
         * 色块正中（正好是小圆点）时按钮收不到 CLICKED，看着就是"点不中"。 */
        lv_obj_t *dot = lv_obj_create(btn);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(dot, 14, 14);
        lv_obj_set_style_bg_color(dot, lv_color_hex(PALETTE[i]), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_center(dot);
    }

    for (int i = 0; i < W_N; i++) {
        lv_obj_t *btn = tool_btn(tool, 152 + i * 28, SWATCH, on_width,
                                 (void *)(intptr_t)i);
        s_width_btn[i] = btn;

        /* 用大小不同的圆点表示粗细（同样要摘掉 CLICKABLE） */
        int d = 4 + i * 5;              /* 4 / 9 / 14 */
        lv_obj_t *dot = lv_obj_create(btn);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(dot, d, d);
        lv_obj_set_style_bg_color(dot, UI_COLOR_TEXT, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_center(dot);
    }

    /* 清空 */
    lv_obj_t *clr = lv_button_create(tool);
    lv_obj_set_size(clr, 64, 26);
    lv_obj_set_pos(clr, 248, BAR_Y + 7);
    lv_obj_set_style_radius(clr, 6, 0);
    lv_obj_set_style_shadow_width(clr, 0, 0);
    lv_obj_set_style_bg_color(clr, UI_COLOR_SURFACE, 0);
    ui_add_click_feedback(clr);
    lv_obj_add_event_cb(clr, on_clear, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *lb = lv_label_create(clr);
        lv_obj_set_style_text_color(lb, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(lb, &font_ui_14, 0);
        lv_label_set_text(lb, "清空");
        lv_obj_center(lb);
    }

    update_tool_ui();

    lv_obj_add_event_cb(root, paint_del_cb, LV_EVENT_DELETE, NULL);
}
