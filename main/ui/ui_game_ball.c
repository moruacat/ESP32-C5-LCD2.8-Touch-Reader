#include "ui_game_ball.h"
#include "ui_common.h"
#include "ui_games.h"
#include "ui_dialog.h"
#include "app_navigator.h"
#include "srv_motion.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 状态行（标题栏以下） + 场地 */
#define BAR_Y        UI_TITLEBAR_H                       /* 28 */
#define BAR_H        24
#define FIELD_Y      (BAR_Y + BAR_H)                     /* 52 */
#define FIELD_W      UI_SCREEN_W                         /* 320 */
#define FIELD_H      (UI_SCREEN_H - FIELD_Y)             /* 188 */

#define BALL_R       9
#define COIN_R       7
#define SPIKE_R      7

#define COIN_N       8
#define SPIKE_N      4
#define LIVES_MAX    3

/*! 物理步长。用固定步长就不必处理 dt 变化，调参数也直观 */
#define TICK_MS      30
/*! 每 1g 倾斜给球的加速度（px/s²）。调这个等于调"手感重力" */
#define ACCEL        2200.0f
/*! 每步速度衰减：稳态速度 = ACCEL×tilt×dt/(1-DAMP) */
#define DAMP         0.94f
/*! 撞墙后的速度保留比例 */
#define BOUNCE       0.72f
#define VMAX         520.0f
/*! 撞钉子后的无敌时长，避免贴着脸连续扣命 */
#define IMMUNE_MS    800

/* 钉子固定四个位置（场地局部坐标），对称摆开 */
static const int s_spike_pos[SPIKE_N][2] = {
    {80, 50}, {240, 50}, {80, 138}, {240, 138},
};

static lv_obj_t *s_root;
static lv_obj_t *s_field;
static lv_obj_t *s_ball;
static lv_obj_t *s_coin[COIN_N];
static lv_obj_t *s_spike[SPIKE_N];

static lv_obj_t *s_score_lb;
static lv_obj_t *s_lives_lb;
static lv_obj_t *s_left_lb;

static float s_bx, s_by;         /*!< 球心（场地局部坐标） */
static float s_vx, s_vy;
static lv_coord_t s_last_x = -1, s_last_y = -1;

static float s_cx[COIN_N], s_cy[COIN_N];
static bool  s_c_eaten[COIN_N];
static int   s_coins_left;

static int  s_score;
static int  s_lives;
static int  s_immune_ms;
static bool s_over;

static void restart_game(void);

/* ===================== 布局 ===================== */

static void coin_place(int i)
{
    /* 随机重试找一块空地：离墙、离钉子、离球起点、离其他金币都留出余量。
     * 撒 8 个金币在 320x188 里，一两次就能中，给足上限兜底即可。 */
    for (int tries = 0; tries < 200; tries++) {
        float x = (float)(rand() % (FIELD_W - COIN_R * 2 - 16)) + COIN_R + 8;
        float y = (float)(rand() % (FIELD_H - COIN_R * 2 - 16)) + COIN_R + 8;

        /* 别贴着球出生的场地中心 */
        float dx = x - FIELD_W / 2.0f;
        float dy = y - FIELD_H / 2.0f;
        if (dx * dx + dy * dy < 55.0f * 55.0f) {
            continue;
        }

        bool bad = false;
        for (int s = 0; s < SPIKE_N && !bad; s++) {
            float sx = (float)s_spike_pos[s][0];
            float sy = (float)s_spike_pos[s][1];
            float ddx = x - sx, ddy = y - sy;
            float need = COIN_R + SPIKE_R + 14.0f;
            if (ddx * ddx + ddy * ddy < need * need) {
                bad = true;
            }
        }
        for (int j = 0; j < i && !bad; j++) {
            float ddx = x - s_cx[j], ddy = y - s_cy[j];
            if (ddx * ddx + ddy * ddy < 40.0f * 40.0f) {
                bad = true;
            }
        }
        if (bad) {
            continue;
        }

        s_cx[i] = x;
        s_cy[i] = y;
        return;
    }

    /* 兜底：均匀铺一行，保证一定能放下去 */
    s_cx[i] = 30.0f + (float)i * 34.0f;
    s_cy[i] = (i % 2 == 0) ? 30.0f : FIELD_H - 30.0f;
}

static void coins_reset(void)
{
    for (int i = 0; i < COIN_N; i++) {
        coin_place(i);
        s_c_eaten[i] = false;
        lv_obj_set_pos(s_coin[i], (lv_coord_t)(s_cx[i] - COIN_R),
                       (lv_coord_t)(s_cy[i] - COIN_R));
        lv_obj_remove_flag(s_coin[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_coins_left = COIN_N;
}

static void ball_center(void)
{
    s_bx = FIELD_W / 2.0f;
    s_by = FIELD_H / 2.0f;
    s_vx = 0.0f;
    s_vy = 0.0f;
    s_immune_ms = 0;

    /* 复位时球一定是实心的（上一轮可能停在无敌的半透明状态） */
    lv_obj_set_style_opa(s_ball, LV_OPA_COVER, 0);
}

static void hud_update(void)
{
    char buf[24];

    snprintf(buf, sizeof(buf), "得分 %d", s_score);
    lv_label_set_text(s_score_lb, buf);

    snprintf(buf, sizeof(buf), "命 %d", s_lives);
    lv_label_set_text(s_lives_lb, buf);

    snprintf(buf, sizeof(buf), "金币 %d", s_coins_left);
    lv_label_set_text(s_left_lb, buf);
}

static void game_reset(void)
{
    s_score      = 0;
    s_lives      = LIVES_MAX;
    s_over       = false;
    ball_center();
    coins_reset();
    hud_update();
}

static void restart_game(void)
{
    if (!ui_game_page_active()) {
        return;
    }
    game_reset();
}

static void game_end(const char *title, lv_color_t color, const char *note)
{
    s_over = true;
    ui_alert_show(title, color, note, "再来一局", ui_game_alert_restart_cb, NULL, 0);
}

/* ===================== 物理 ===================== */

static void ball_render(void)
{
    lv_coord_t x = (lv_coord_t)(s_bx - BALL_R);
    lv_coord_t y = (lv_coord_t)(s_by - BALL_R);
    if (x == s_last_x && y == s_last_y) {
        return;                 /* 没动过就别 set_pos，省一次重绘 */
    }
    lv_obj_set_pos(s_ball, x, y);
    s_last_x = x;
    s_last_y = y;
}

static void hit_spike(int i)
{
    float sx = (float)s_spike_pos[i][0];
    float sy = (float)s_spike_pos[i][1];

    float dx = s_bx - sx;
    float dy = s_by - sy;
    float d2 = dx * dx + dy * dy;
    const float need = (float)(BALL_R + SPIKE_R);
    if (d2 >= need * need || d2 < 0.0001f) {
        return;
    }

    float d  = sqrtf(d2);
    float nx = dx / d;
    float ny = dy / d;

    /* 先推出重叠，否则下一帧还判定在钉子里，会被反复扣命 */
    s_bx = sx + nx * need;
    s_by = sy + ny * need;

    /* 沿法线反射，再补一点推力，免得贴在钉子上磨 */
    float vn = s_vx * nx + s_vy * ny;
    if (vn < 0.0f) {
        s_vx -= 2.0f * vn * nx * BOUNCE;
        s_vy -= 2.0f * vn * ny * BOUNCE;
    }
    s_vx += nx * 70.0f;
    s_vy += ny * 70.0f;

    if (s_immune_ms > 0) {
        return;                 /* 无敌期内只弹开，不扣命 */
    }

    s_immune_ms = IMMUNE_MS;
    s_lives--;
    hud_update();

    if (s_lives <= 0) {
        char note[32];
        snprintf(note, sizeof(note), "得分 %d", s_score);
        game_end("撞上钉子了", UI_COLOR_TEMP, note);
    }
}

static void eat_coin(void)
{
    for (int i = 0; i < COIN_N; i++) {
        if (s_c_eaten[i]) {
            continue;
        }

        float dx = s_bx - s_cx[i];
        float dy = s_by - s_cy[i];
        float need = (float)(BALL_R + COIN_R);
        if (dx * dx + dy * dy >= need * need) {
            continue;
        }

        s_c_eaten[i] = true;
        lv_obj_add_flag(s_coin[i], LV_OBJ_FLAG_HIDDEN);
        s_coins_left--;
        s_score += 10;
        hud_update();

        if (s_coins_left <= 0) {
            char note[32];
            snprintf(note, sizeof(note), "得分 %d", s_score);
            game_end("金币吃完了", UI_COLOR_OK, note);
            return;
        }
    }
}

static void ball_tick(lv_timer_t *timer)
{
    (void)timer;

    if (s_root == NULL || s_over) {
        return;
    }

    float tx = 0.0f, ty = 0.0f;
    if (srv_motion_read_tilt(&tx, &ty) != ESP_OK) {
        return;                 /* 传感器读失败这一帧就不动，别把球甩出去 */
    }

    const float dt = (float)TICK_MS / 1000.0f;

    s_vx = (s_vx + tx * ACCEL * dt) * DAMP;
    s_vy = (s_vy + ty * ACCEL * dt) * DAMP;

    if (s_vx >  VMAX) s_vx =  VMAX;
    if (s_vx < -VMAX) s_vx = -VMAX;
    if (s_vy >  VMAX) s_vy =  VMAX;
    if (s_vy < -VMAX) s_vy = -VMAX;

    s_bx += s_vx * dt;
    s_by += s_vy * dt;

    /* 四壁反弹 */
    if (s_bx < BALL_R) {
        s_bx = BALL_R;
        s_vx = -s_vx * BOUNCE;
    } else if (s_bx > FIELD_W - BALL_R) {
        s_bx = FIELD_W - BALL_R;
        s_vx = -s_vx * BOUNCE;
    }
    if (s_by < BALL_R) {
        s_by = BALL_R;
        s_vy = -s_vy * BOUNCE;
    } else if (s_by > FIELD_H - BALL_R) {
        s_by = FIELD_H - BALL_R;
        s_vy = -s_vy * BOUNCE;
    }

    if (s_immune_ms > 0) {
        s_immune_ms -= TICK_MS;
        /* 无敌期间让球半透明，玩家一眼能看出现在撞了不扣命 */
        lv_obj_set_style_opa(s_ball, s_immune_ms > 0 ? LV_OPA_50 : LV_OPA_COVER, 0);
    }

    for (int i = 0; i < SPIKE_N && !s_over; i++) {
        hit_spike(i);
    }
    if (!s_over) {
        eat_coin();
    }

    ball_render();
}

/* ===================== 方向菜单 ===================== */

static void motion_apply(void)
{
    /* 改完方向把速度清零，免得上一把的惯性把球直接甩到钉子上 */
    s_vx = 0.0f;
    s_vy = 0.0f;
}

static void on_dir_menu(int idx, void *arg)
{
    (void)arg;

    const bool sw = srv_motion_swap_xy();
    const bool fx = srv_motion_flip_x();
    const bool fy = srv_motion_flip_y();

    switch (idx) {
    case 0:
        /* 手感整体差了 90° 时用这个：球往上下走而不是左右 */
        srv_motion_set_flip(!sw, fx, fy);
        break;
    case 1:
        srv_motion_set_flip(sw, !fx, fy);
        break;
    case 2:
        srv_motion_set_flip(sw, fx, !fy);
        break;
    case 3:
        srv_motion_set_flip(false, false, false);
        break;
    default:
        return;
    }

    motion_apply();
}

static void on_dir_btn(lv_event_t *e)
{
    (void)e;
    /* 顺序按"最可能用到的排前面"：轴反了是最常见的，先给交换轴 */
    static const ui_menu_item_t items[] = {
        {"交换轴", NULL},
        {"左右翻转", NULL},
        {"上下翻转", NULL},
        {"重设方向", NULL},
    };
    ui_menu_show("倾斜方向不对就调一下", items, 4, on_dir_menu, NULL);
}

/* ===================== 页面 ===================== */

static void ball_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    if (lv_event_get_target(e) != s_root) {
        return;
    }

    s_root     = NULL;
    s_field    = NULL;
    s_ball     = NULL;
    s_score_lb = NULL;
    s_lives_lb = NULL;
    s_left_lb  = NULL;
    s_last_x   = -1;
    s_last_y   = -1;
    for (int i = 0; i < COIN_N; i++) {
        s_coin[i] = NULL;
    }
    for (int i = 0; i < SPIKE_N; i++) {
        s_spike[i] = NULL;
    }
}

void ui_game_ball_create(lv_obj_t *root)
{
    s_root = root;
    ui_game_page_enter(root, PAGE_GAME_BALL, restart_game);

    ui_game_bar_create(root, "重力球");

    /* ===== 状态行：得分 / 命 / 金币 / 方向 ===== */
    s_score_lb = lv_label_create(root);
    lv_obj_set_style_text_color(s_score_lb, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_score_lb, &font_ui_14, 0);
    lv_label_set_text(s_score_lb, "得分 0");
    lv_obj_set_pos(s_score_lb, 8, BAR_Y + 4);

    s_lives_lb = lv_label_create(root);
    lv_obj_set_style_text_color(s_lives_lb, UI_COLOR_TEMP, 0);
    lv_obj_set_style_text_font(s_lives_lb, &font_ui_14, 0);
    lv_label_set_text(s_lives_lb, "命 3");
    lv_obj_set_pos(s_lives_lb, 92, BAR_Y + 4);

    s_left_lb = lv_label_create(root);
    lv_obj_set_style_text_color(s_left_lb, lv_color_hex(0xE8C84D), 0);
    lv_obj_set_style_text_font(s_left_lb, &font_ui_14, 0);
    lv_label_set_text(s_left_lb, "金币 8");
    lv_obj_set_pos(s_left_lb, 158, BAR_Y + 4);

    lv_obj_t *dir = lv_button_create(root);
    lv_obj_set_size(dir, 60, 20);
    lv_obj_set_pos(dir, UI_SCREEN_W - 68, BAR_Y + 2);
    lv_obj_set_style_radius(dir, 6, 0);
    lv_obj_set_style_bg_color(dir, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_shadow_width(dir, 0, 0);
    ui_add_click_feedback(dir);
    lv_obj_add_event_cb(dir, on_dir_btn, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *lb = lv_label_create(dir);
        lv_obj_set_style_text_color(lb, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(lb, &font_ui_14, 0);
        lv_label_set_text(lb, "方向");
        lv_obj_center(lb);
    }

    /* ===== 场地 ===== */
    s_field = lv_obj_create(root);
    lv_obj_remove_flag(s_field, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_field, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_field, FIELD_W, FIELD_H);
    lv_obj_set_pos(s_field, 0, FIELD_Y);
    lv_obj_set_style_bg_color(s_field, lv_color_hex(0x0E1318), 0);
    lv_obj_set_style_bg_opa(s_field, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_field, 0, 0);
    lv_obj_set_style_radius(s_field, 0, 0);
    lv_obj_set_style_pad_all(s_field, 0, 0);

    /* 钉子：暗红圆 + 亮边，看着比金币"危险" */
    for (int i = 0; i < SPIKE_N; i++) {
        lv_obj_t *sp = lv_obj_create(s_field);
        lv_obj_remove_flag(sp, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(sp, SPIKE_R * 2, SPIKE_R * 2);
        lv_obj_set_pos(sp, s_spike_pos[i][0] - SPIKE_R, s_spike_pos[i][1] - SPIKE_R);
        lv_obj_set_style_bg_color(sp, lv_color_hex(0x5A1F22), 0);
        lv_obj_set_style_bg_opa(sp, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(sp, 2, 0);
        lv_obj_set_style_border_color(sp, UI_COLOR_TEMP, 0);
        lv_obj_set_style_radius(sp, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(sp, 0, 0);
        s_spike[i] = sp;
    }

    /* 金币 */
    for (int i = 0; i < COIN_N; i++) {
        lv_obj_t *c = lv_obj_create(s_field);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(c, COIN_R * 2, COIN_R * 2);
        lv_obj_set_style_bg_color(c, lv_color_hex(0xE8C84D), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(c, 0, 0);
        lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(c, 0, 0);
        s_coin[i] = c;
    }

    /* 球 */
    s_ball = lv_obj_create(s_field);
    lv_obj_remove_flag(s_ball, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_ball, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_ball, BALL_R * 2, BALL_R * 2);
    lv_obj_set_style_bg_color(s_ball, UI_COLOR_PRIMARY, 0);
    lv_obj_set_style_bg_opa(s_ball, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ball, 2, 0);
    lv_obj_set_style_border_color(s_ball, lv_color_hex(0x7FD4FF), 0);
    lv_obj_set_style_radius(s_ball, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_ball, 0, 0);

    if (!srv_motion_ready()) {
        /* 传感器没起来就别开跑：球会一直停在中心，看着像卡死 */
        s_over = true;
        ui_alert_show("重力感应不可用", UI_COLOR_WARN,
                      "没读到六轴传感器，这个游戏玩不了",
                      "返回", NULL, NULL, 0);
        lv_obj_add_event_cb(root, ball_del_cb, LV_EVENT_DELETE, NULL);
        return;
    }

    game_reset();

    /* 定时器与页面同生命周期：页面销毁时自动删 */
    ui_game_timer_bind(root, lv_timer_create(ball_tick, TICK_MS, NULL));
    lv_obj_add_event_cb(root, ball_del_cb, LV_EVENT_DELETE, NULL);
}
