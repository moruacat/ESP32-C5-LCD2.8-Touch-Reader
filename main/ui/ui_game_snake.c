#include "ui_game_snake.h"
#include "ui_common.h"
#include "ui_games.h"
#include "ui_dialog.h"
#include "app_navigator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(lv_font_montserrat_24);

/* 棋盘：20x13 格 x 16px = 320x208，正好铺满标题栏以下 */
#define CELL        16
#define COLS        20
#define ROWS        13
#define BOARD_H     (ROWS * CELL)

/*! 蛇身对象池上限。20x13 全铺满也只有 260 节，实际玩不到 100 节 */
#define MAX_LEN     100

#define TICK_START  250      /*!< 初始每步间隔(ms) */
#define TICK_MIN    90       /*!< 最快间隔 */
#define TICK_STEP   12       /*!< 每吃一个加快多少 */

static lv_obj_t *s_root;
static lv_obj_t *s_seg[MAX_LEN];
static lv_obj_t *s_food;
static lv_obj_t *s_score_lb;

static int8_t s_x[MAX_LEN];      /*!< [0] 是蛇头 */
static int8_t s_y[MAX_LEN];
static int    s_len;

static int8_t s_dx, s_dy;        /*!< 当前方向 */
static int8_t s_ndx, s_ndy;      /*!< 下一次 tick 生效的方向 */

static int  s_fx, s_fy;
static int  s_score;
static int  s_period;            /*!< 当前每步间隔(ms)，吃一个加快一点 */
static bool s_dead;

static void restart_game(void);

/* ===================== 规则 ===================== */

static void render(void)
{
    for (int i = 0; i < MAX_LEN; i++) {
        lv_obj_t *o = s_seg[i];
        if (i >= s_len) {
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);

        /* 位置没变就别 set_pos：那会白白标脏一小块区域，蛇越长越明显 */
        lv_coord_t nx = (lv_coord_t)s_x[i] * CELL;
        lv_coord_t ny = (lv_coord_t)s_y[i] * CELL;
        if (lv_obj_get_x(o) != nx || lv_obj_get_y(o) != ny) {
            lv_obj_set_pos(o, nx, ny);
        }
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", s_score);
    lv_label_set_text(s_score_lb, buf);
}

static void place_food(void)
{
    /* 随机重试找空格：空位比例很高，实际一两次就中 */
    for (int tries = 0; tries < 400; tries++) {
        int x = rand() % COLS;
        int y = rand() % ROWS;

        bool on_snake = false;
        for (int i = 0; i < s_len; i++) {
            if (s_x[i] == x && s_y[i] == y) {
                on_snake = true;
                break;
            }
        }
        if (!on_snake) {
            s_fx = x;
            s_fy = y;
            lv_obj_set_pos(s_food, x * CELL, y * CELL);
            return;
        }
    }
}

static void snake_reset(void)
{
    s_len = 3;
    const int cy = ROWS / 2;
    for (int i = 0; i < s_len; i++) {
        s_x[i] = (int8_t)(6 - i);          /* 头在 6，朝右 */
        s_y[i] = (int8_t)cy;
    }

    s_dx = 1;  s_dy = 0;
    s_ndx = 1; s_ndy = 0;
    s_score = 0;
    s_dead  = false;

    place_food();
    render();
}

static void snake_die(void)
{
    s_dead = true;

    char note[32];
    snprintf(note, sizeof(note), "得分 %d", s_score);
    ui_alert_show("撞上了", UI_COLOR_TEMP, note, "重新开始",
                  ui_game_alert_restart_cb, NULL, 0);
}

static void snake_tick(lv_timer_t *timer)
{
    if (s_root == NULL || s_dead) {
        return;
    }

    /* 重开后分数归零，顺便把速度也拉回初速 */
    if (s_score == 0 && s_period != TICK_START) {
        s_period = TICK_START;
        lv_timer_set_period(timer, (uint32_t)s_period);
    }

    s_dx = s_ndx;
    s_dy = s_ndy;

    int hx = s_x[0] + s_dx;
    int hy = s_y[0] + s_dy;

    if (hx < 0 || hx >= COLS || hy < 0 || hy >= ROWS) {
        snake_die();
        return;
    }

    const bool eat = (hx == s_fx && hy == s_fy);

    /* 撞自己：不吃食物时尾巴会让出来，所以尾巴那一格不算 */
    const int limit = eat ? s_len : s_len - 1;
    for (int i = 0; i < limit; i++) {
        if (s_x[i] == hx && s_y[i] == hy) {
            snake_die();
            return;
        }
    }

    int new_len = (eat && s_len < MAX_LEN) ? s_len + 1 : s_len;
    for (int i = new_len - 1; i > 0; i--) {
        s_x[i] = s_x[i - 1];
        s_y[i] = s_y[i - 1];
    }
    s_x[0] = (int8_t)hx;
    s_y[0] = (int8_t)hy;
    s_len  = new_len;

    if (eat) {
        s_score += 10;
        place_food();

        s_period -= TICK_STEP;
        if (s_period < TICK_MIN) {
            s_period = TICK_MIN;
        }
        lv_timer_set_period(timer, (uint32_t)s_period);
    }

    render();
}

static void restart_game(void)
{
    if (!ui_game_page_active()) {
        return;
    }
    snake_reset();               /* 分数归零，速度由 tick 自己恢复初速 */
}

/* ===================== 触摸：滑动转向 ===================== */

/* 方向由 ui_games 统一判定：0=上 1=下 2=左 3=右 */
static void on_swipe(int dir, void *arg)
{
    (void)arg;
    if (s_dead) {
        return;
    }

    int8_t nx = 0;
    int8_t ny = 0;
    switch (dir) {
    case 0:  ny = -1; break;
    case 1:  ny = 1;  break;
    case 2:  nx = -1; break;
    default: nx = 1;  break;
    }

    /* 不能掉头：既不能反当前方向，也不能反还没生效的方向
     * （一个 tick 内连滑两次，比如 上→左→右，第二次反的是 ndir）。 */
    if ((nx == -s_dx && ny == -s_dy) || (nx == -s_ndx && ny == -s_ndy)) {
        return;
    }

    s_ndx = nx;
    s_ndy = ny;
}

/* ===================== 页面 ===================== */

static void snake_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    if (lv_event_get_target(e) != s_root) {
        return;
    }

    s_root     = NULL;
    s_food     = NULL;
    s_score_lb = NULL;
    for (int i = 0; i < MAX_LEN; i++) {
        s_seg[i] = NULL;
    }
}

void ui_game_snake_create(lv_obj_t *root)
{
    s_root = root;
    ui_game_page_enter(root, PAGE_GAME_SNAKE, restart_game);

    lv_obj_t *bar = ui_game_bar_create(root, "贪吃蛇");

    /* 分数挂在标题栏上（棋盘铺满了标题栏以下，没地方放） */
    s_score_lb = lv_label_create(bar);
    lv_obj_set_style_text_color(s_score_lb, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_score_lb, &lv_font_montserrat_24, 0);
    lv_label_set_text(s_score_lb, "0");
    lv_obj_align(s_score_lb, LV_ALIGN_RIGHT_MID, -34, 0);

    /* 棋盘、网格线、食物、蛇身都是 lv_obj_create() 出来的，默认带
     * LV_OBJ_FLAG_CLICKABLE。LVGL 的命中测试是「从最后一个子对象往前找，
     * 谁先命中且可点击就归谁」，命中后**不再往父对象传** —— 不摘掉这些标志，
     * 按下会被蛇身/格子接走，挂在 root 上的 PRESSED/RELEASED 永远收不到，
     * 表现就是完全没法转向。摘掉后按下落到 root，滑出棋盘也算数。 */
    lv_obj_t *board = lv_obj_create(root);
    lv_obj_remove_flag(board, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(board, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(board, UI_SCREEN_W, BOARD_H);
    lv_obj_set_pos(board, 0, UI_TITLEBAR_H);
    lv_obj_set_style_bg_color(board, lv_color_hex(0x0B0F14), 0);
    lv_obj_set_style_bg_opa(board, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(board, 0, 0);
    lv_obj_set_style_radius(board, 0, 0);
    lv_obj_set_style_pad_all(board, 0, 0);

    /* 淡网格：只在创建时画一次，之后不参与重绘 */
    for (int i = 1; i < COLS; i++) {
        lv_obj_t *ln = lv_obj_create(board);
        lv_obj_remove_flag(ln, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(ln, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(ln, 1, BOARD_H);
        lv_obj_set_pos(ln, i * CELL, 0);
        lv_obj_set_style_bg_color(ln, lv_color_hex(0x16202A), 0);
        lv_obj_set_style_bg_opa(ln, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ln, 0, 0);
        lv_obj_set_style_radius(ln, 0, 0);
        lv_obj_set_style_pad_all(ln, 0, 0);
    }
    for (int i = 1; i < ROWS; i++) {
        lv_obj_t *ln = lv_obj_create(board);
        lv_obj_remove_flag(ln, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(ln, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(ln, UI_SCREEN_W, 1);
        lv_obj_set_pos(ln, 0, i * CELL);
        lv_obj_set_style_bg_color(ln, lv_color_hex(0x16202A), 0);
        lv_obj_set_style_bg_opa(ln, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ln, 0, 0);
        lv_obj_set_style_radius(ln, 0, 0);
        lv_obj_set_style_pad_all(ln, 0, 0);
    }

    /* 食物 */
    s_food = lv_obj_create(board);
    lv_obj_remove_flag(s_food, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_food, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_food, CELL - 1, CELL - 1);
    lv_obj_set_style_bg_color(s_food, UI_COLOR_TEMP, 0);
    lv_obj_set_style_bg_opa(s_food, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_food, 0, 0);
    lv_obj_set_style_radius(s_food, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_food, 0, 0);

    /* 蛇身对象池：先建好再按需显示，避免每步创建/删除对象 */
    for (int i = 0; i < MAX_LEN; i++) {
        lv_obj_t *seg = lv_obj_create(board);
        lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(seg, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(seg, CELL - 1, CELL - 1);
        lv_obj_set_style_bg_color(seg, (i == 0) ? lv_color_hex(0x3BD07A)
                                                : UI_COLOR_OK, 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(seg, 0, 0);
        lv_obj_set_style_radius(seg, 3, 0);
        lv_obj_set_style_pad_all(seg, 0, 0);
        lv_obj_add_flag(seg, LV_OBJ_FLAG_HIDDEN);
        s_seg[i] = seg;
    }

    s_period = TICK_START;
    snake_reset();

    /* 定时器与页面同生命周期：页面销毁时自动删 */
    ui_game_timer_bind(root, lv_timer_create(snake_tick, TICK_START, NULL));

    ui_game_swipe_enable(root, on_swipe, NULL);
    lv_obj_add_event_cb(root, snake_del_cb, LV_EVENT_DELETE, NULL);
}