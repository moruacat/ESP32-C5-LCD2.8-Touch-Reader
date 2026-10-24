#include "ui_game_soccer.h"
#include "ui_common.h"
#include "ui_games.h"
#include "ui_dialog.h"
#include "app_navigator.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* 状态行（标题栏以下） + 场地 */
#define BAR_Y        UI_TITLEBAR_H                       /* 28 */
#define BAR_H        24
#define FIELD_Y      (BAR_Y + BAR_H)                     /* 52 */
#define FIELD_W      UI_SCREEN_W                         /* 320 */
#define FIELD_H      (UI_SCREEN_H - FIELD_Y)             /* 188 */

#define BALL_R       7
#define PAD_R        15
#define KEEPER_R     13

/*! 球门开口高度，居中开在右侧墙上 */
#define GOAL_H       76
#define GOAL_TOP     ((FIELD_H - GOAL_H) / 2)
#define GOAL_BOT     ((FIELD_H + GOAL_H) / 2)

/*! 守门员在球门左侧多远处来回跑 */
#define KEEPER_X     (FIELD_W - 46)

/*! 进球后球在场中心停多久再放出去 */
#define HOLD_MS      450

#define TICK_MS      30
#define ROUND_MS     60000      /*!< 一局 60 秒 */

#define DAMP         0.988f     /*!< 每步速度衰减（冰面感，衰减要小） */
#define VMAX         480.0f
#define WALL_BOUNCE  0.86f
#define PAD_PUSH     1.7f       /*!< 球拍速度传给球的比例 */
#define PAD_BASE     55.0f      /*!< 球拍碰到球时额外给的基础推力 */

#define KEEPER_V0    70.0f      /*!< 守门员初始速度 px/s */
#define KEEPER_VMAX  175.0f

static lv_obj_t *s_root;
static lv_obj_t *s_field;
static lv_obj_t *s_ball;
static lv_obj_t *s_pad;
static lv_obj_t *s_keeper;
static lv_obj_t *s_goal;

static lv_obj_t *s_score_lb;
static lv_obj_t *s_time_lb;

static float s_bx, s_by, s_vx, s_vy;
static lv_coord_t s_last_bx = -1, s_last_by = -1;

static float s_px, s_py;                 /*!< 球拍中心 */
static float s_pvx, s_pvy;               /*!< 球拍速度（由手指位移算） */
static lv_coord_t s_last_px = -1, s_last_py = -1;

static float s_ky, s_kvy;                /*!< 守门员 y 与速度 */

static int  s_score;
static int  s_left_ms;
static bool s_over;
static int  s_hold_ms;                   /*!< >0 时球停在中圈不动，等玩家看清 */
static bool s_was_pressed;               /*!< 上一帧手指是否按着（用来识别"刚按下"那一帧） */
static void restart_game(void);
static void touch_follow(void);

/* ===================== 工具 ===================== */

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void hud_update(void)
{
    char buf[24];

    snprintf(buf, sizeof(buf), "得分 %d", s_score);
    lv_label_set_text(s_score_lb, buf);

    snprintf(buf, sizeof(buf), "时间 %d", (s_left_ms + 999) / 1000);
    lv_label_set_text(s_time_lb, buf);
}

static void ball_reset(void)
{
    s_bx = FIELD_W / 2.0f;
    s_by = FIELD_H / 2.0f;

    /* 往右侧（球门那边）弹出去，纵向随机一点，避免每次都同一条路 */
    s_vx = 150.0f;
    s_vy = (float)(rand() % 200 - 100);

    s_hold_ms = HOLD_MS;    /* 先停一下，让玩家看清球在哪 */
}

static void pad_reset(void)
{
    s_px = 80.0f;
    s_py = FIELD_H / 2.0f;
    s_pvx = 0.0f;
    s_pvy = 0.0f;
    s_last_px = -1;
    s_last_py = -1;
}

static void game_reset(void)
{
    s_score   = 0;
    s_left_ms = ROUND_MS;
    s_over    = false;
    s_was_pressed = false;

    s_ky  = FIELD_H / 2.0f;
    s_kvy = KEEPER_V0;

    ball_reset();
    pad_reset();
    hud_update();
}

static void restart_game(void)
{
    if (!ui_game_page_active()) {
        return;
    }
    game_reset();
}

/* ===================== 渲染 ===================== */

static void render(void)
{
    lv_coord_t bx = (lv_coord_t)(s_bx - BALL_R);
    lv_coord_t by = (lv_coord_t)(s_by - BALL_R);
    if (bx != s_last_bx || by != s_last_by) {
        lv_obj_set_pos(s_ball, bx, by);
        s_last_bx = bx;
        s_last_by = by;
    }

    lv_coord_t px = (lv_coord_t)(s_px - PAD_R);
    lv_coord_t py = (lv_coord_t)(s_py - PAD_R);
    if (px != s_last_px || py != s_last_py) {
        lv_obj_set_pos(s_pad, px, py);
        s_last_px = px;
        s_last_py = py;
    }

    lv_obj_set_y(s_keeper, (lv_coord_t)(s_ky - KEEPER_R));
}

/* ===================== 物理 ===================== */

/* 拨球：球拍按位置推开球，并把自身的移动速度传一部分给球 */
static void pad_hit(void)
{
    float dx = s_bx - s_px;
    float dy = s_by - s_py;
    float d2 = dx * dx + dy * dy;
    const float need = (float)(BALL_R + PAD_R);
    if (d2 >= need * need) {
        return;
    }

    float d = sqrtf(d2);
    if (d < 0.001f) {
        /* 球心正好压在球拍中心：随便挑一个方向推开，否则除零 */
        dx = 1.0f;
        dy = 0.0f;
        d  = 1.0f;
    }

    float nx = dx / d;
    float ny = dy / d;

    /* 先把球顶出重叠，不然下一帧还判定在球拍里，会被反复加速 */
    s_bx = s_px + nx * need;
    s_by = s_py + ny * need;

    float vpad = s_pvx * nx + s_pvy * ny;    /* 球拍速度在法线上的分量 */
    float vn   = s_vx * nx + s_vy * ny;      /* 球速在法线上的分量 */

    /* 只在"球拍朝球推过去"的时候改球速：球自己弹回来撞上静止的手指，
     * 不应该被凭空加速 */
    if (vn < vpad) {
        float add = (vpad - vn) * PAD_PUSH + PAD_BASE;
        s_vx += add * nx;
        s_vy += add * ny;
    }
}

/* 守门员：在球门前来回跑，速度随进球数提高 */
static void keeper_tick(float dt)
{
    s_ky += s_kvy * dt;

    const float lo = KEEPER_R + 4.0f;
    const float hi = FIELD_H - KEEPER_R - 4.0f;
    if (s_ky < lo) {
        s_ky = lo;
        s_kvy = -s_kvy;
    } else if (s_ky > hi) {
        s_ky = hi;
        s_kvy = -s_kvy;
    }
}

static void keeper_hit(void)
{
    float dx = s_bx - (float)KEEPER_X;
    float dy = s_by - s_ky;
    float d2 = dx * dx + dy * dy;
    const float need = (float)(BALL_R + KEEPER_R);
    if (d2 >= need * need) {
        return;
    }

    float d = sqrtf(d2);
    if (d < 0.001f) {
        dx = -1.0f;
        dy = 0.0f;
        d  = 1.0f;
    }

    float nx = dx / d;
    float ny = dy / d;

    s_bx = (float)KEEPER_X + nx * need;
    s_by = s_ky + ny * need;

    /* 反射，再补一点向外的推力，免得球贴在守门员身上不动 */
    float vn = s_vx * nx + s_vy * ny;
    if (vn < 0.0f) {
        s_vx -= 2.0f * vn * nx * 0.9f;
        s_vy -= 2.0f * vn * ny * 0.9f;
    }
    s_vx -= 90.0f;          /* 被扑出来的球总是往左飞 */
}

static void goal_scored(void)
{
    s_score++;
    hud_update();

    /* 每进一个球守门员跑快一点，不然 60 秒能刷太多 */
    float v = KEEPER_V0 + (float)s_score * 18.0f;
    if (v > KEEPER_VMAX) {
        v = KEEPER_VMAX;
    }
    s_kvy = (s_kvy < 0.0f) ? -v : v;

    ball_reset();
}

static void game_over(void)
{
    s_over = true;

    char note[40];
    snprintf(note, sizeof(note), "60 秒进了 %d 球", s_score);
    ui_alert_show("时间到", UI_COLOR_PRIMARY, note, "再来一局",
                  ui_game_alert_restart_cb, NULL, 0);
}

static void soccer_tick(lv_timer_t *timer)
{
    (void)timer;

    if (s_root == NULL || s_over) {
        return;
    }

    const float dt = (float)TICK_MS / 1000.0f;

    s_left_ms -= TICK_MS;
    if (s_left_ms <= 0) {
        s_left_ms = 0;
        hud_update();
        game_over();
        return;
    }
    hud_update();

    keeper_tick(dt);

    /* 球拍指哪打哪：直接读指针设备当前状态，不依赖 PRESSING 事件 ——
     * 事件要等指针挪够一定距离才发，慢速拖动时球拍会跟不上手指。 */
    touch_follow();

    if (s_hold_ms > 0) {
        /* 进球后的短暂停球：球摆在中圈不动，等玩家看清再放出去 */
        s_hold_ms -= TICK_MS;
        s_pvx = 0.0f;
        s_pvy = 0.0f;
        render();
        return;
    }

    s_vx *= DAMP;
    s_vy *= DAMP;

    if (s_vx >  VMAX) s_vx =  VMAX;
    if (s_vx < -VMAX) s_vx = -VMAX;
    if (s_vy >  VMAX) s_vy =  VMAX;
    if (s_vy < -VMAX) s_vy = -VMAX;

    s_bx += s_vx * dt;
    s_by += s_vy * dt;

    /* 上下壁 */
    if (s_by < BALL_R) {
        s_by = BALL_R;
        s_vy = -s_vy * WALL_BOUNCE;
    } else if (s_by > FIELD_H - BALL_R) {
        s_by = FIELD_H - BALL_R;
        s_vy = -s_vy * WALL_BOUNCE;
    }

    /* 左壁 */
    if (s_bx < BALL_R) {
        s_bx = BALL_R;
        s_vx = -s_vx * WALL_BOUNCE;
    }

    /* 右壁：开口范围内算进球，否则反弹 */
    if (s_bx > FIELD_W - BALL_R) {
        if (s_by > GOAL_TOP && s_by < GOAL_BOT) {
            goal_scored();
            render();
            return;
        }
        s_bx = FIELD_W - BALL_R;
        s_vx = -s_vx * WALL_BOUNCE;
    }

    keeper_hit();
    pad_hit();

    /* 球拍速度由位移算，算完清零，下一帧重新累积 */
    s_pvx = 0.0f;
    s_pvy = 0.0f;

    render();
}

/* ===================== 触摸：球拍跟手 ===================== */

/* 读指针设备：按着的话把当前坐标填进 *p 并返回 true */
static bool touch_pos(lv_point_t *p)
{
    for (lv_indev_t *indev = lv_indev_get_next(NULL);
         indev != NULL;
         indev = lv_indev_get_next(indev)) {

        if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
            continue;
        }
        if (lv_indev_get_state(indev) != LV_INDEV_STATE_PRESSED) {
            continue;
        }

        lv_indev_get_point(indev, p);
        return true;
    }
    return false;
}

/**
 * @brief 让球拍跟上手指
 *
 * 每帧调一次。刚按下那一帧球拍直接"瞬移"到手指位置并把速度清零 ——
 * 不这么做的话，第一帧的位移会被当成一次极快的挥拍，球会被抽飞。
 */
static void touch_follow(void)
{
    lv_point_t p;

    if (!touch_pos(&p)) {
        s_was_pressed = false;
        s_pvx = 0.0f;
        s_pvy = 0.0f;
        return;
    }

    float nx = clampf((float)p.x, PAD_R, FIELD_W - PAD_R);
    float ny = clampf((float)(p.y - FIELD_Y), PAD_R, FIELD_H - PAD_R);

    if (!s_was_pressed) {
        s_was_pressed = true;
        s_px = nx;
        s_py = ny;
        s_pvx = 0.0f;
        s_pvy = 0.0f;
        s_last_px = -1;             /* 强制下一次渲染立刻归位 */
        return;
    }

    /* 球拍速度取"这一小段位移 / 一帧时长"。
     * 手指在一帧内动很远就说明甩得很快，传给球的速度自然就大。 */
    const float dt = (float)TICK_MS / 1000.0f;
    s_pvx = (nx - s_px) / dt;
    s_pvy = (ny - s_py) / dt;

    s_px = nx;
    s_py = ny;
}

/* ===================== 页面 ===================== */

static void soccer_del_cb(lv_event_t *e)
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
    s_pad      = NULL;
    s_keeper   = NULL;
    s_goal     = NULL;
    s_score_lb = NULL;
    s_time_lb  = NULL;
    s_last_bx  = -1;
    s_last_by  = -1;
    s_last_px  = -1;
    s_last_py  = -1;
}

void ui_game_soccer_create(lv_obj_t *root)
{
    s_root = root;
    ui_game_page_enter(root, PAGE_GAME_SOCCER, restart_game);

    ui_game_bar_create(root, "足球");

    /* ===== 状态行 ===== */
    s_score_lb = lv_label_create(root);
    lv_obj_set_style_text_color(s_score_lb, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_score_lb, &font_ui_14, 0);
    lv_label_set_text(s_score_lb, "得分 0");
    lv_obj_set_pos(s_score_lb, 8, BAR_Y + 4);

    s_time_lb = lv_label_create(root);
    lv_obj_set_style_text_color(s_time_lb, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_time_lb, &font_ui_14, 0);
    lv_label_set_text(s_time_lb, "时间 60");
    lv_obj_set_pos(s_time_lb, 92, BAR_Y + 4);

    {
        lv_obj_t *tip = lv_label_create(root);
        lv_obj_set_style_text_color(tip, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_font(tip, &font_ui_14, 0);
        lv_label_set_text(tip, "手指拨球进门");
        lv_obj_set_pos(tip, 176, BAR_Y + 4);
    }

    /* ===== 场地 =====
     *
     * 场地和里面所有元素都不接收点击：球拍是由 tick 直接读指针设备驱动的
     * （见 touch_follow），不靠事件。这样做的另一个好处是页面根对象上没有
     * 可点击的东西抢事件，通用手势能正常生效（横向拨球不会误触返回）——
     * 不过足球还是排除在通用手势之外了，免得在场地里横滑直接退出。 */
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

    /* 中线 */
    {
        lv_obj_t *mid = lv_obj_create(s_field);
        lv_obj_remove_flag(mid, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(mid, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(mid, 2, FIELD_H);
        lv_obj_set_pos(mid, FIELD_W / 2 - 1, 0);
        lv_obj_set_style_bg_color(mid, lv_color_hex(0x1B222B), 0);
        lv_obj_set_style_bg_opa(mid, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(mid, 0, 0);
        lv_obj_set_style_radius(mid, 0, 0);
        lv_obj_set_style_pad_all(mid, 0, 0);
    }

    /* 球门：右侧墙上一条亮色竖条，开口多高一眼能看出来 */
    s_goal = lv_obj_create(s_field);
    lv_obj_remove_flag(s_goal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_goal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_goal, 5, GOAL_H);
    lv_obj_set_pos(s_goal, FIELD_W - 5, GOAL_TOP);
    lv_obj_set_style_bg_color(s_goal, UI_COLOR_OK, 0);
    lv_obj_set_style_bg_opa(s_goal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_goal, 0, 0);
    lv_obj_set_style_radius(s_goal, 0, 0);
    lv_obj_set_style_pad_all(s_goal, 0, 0);

    /* 守门员 */
    s_keeper = lv_obj_create(s_field);
    lv_obj_remove_flag(s_keeper, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_keeper, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_keeper, KEEPER_R * 2, KEEPER_R * 2);
    lv_obj_set_x(s_keeper, KEEPER_X - KEEPER_R);
    lv_obj_set_style_bg_color(s_keeper, lv_color_hex(0xF5A623), 0);
    lv_obj_set_style_bg_opa(s_keeper, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_keeper, 2, 0);
    lv_obj_set_style_border_color(s_keeper, lv_color_hex(0x8A5B12), 0);
    lv_obj_set_style_radius(s_keeper, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_keeper, 0, 0);

    /* 球 */
    s_ball = lv_obj_create(s_field);
    lv_obj_remove_flag(s_ball, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_ball, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_ball, BALL_R * 2, BALL_R * 2);
    lv_obj_set_style_bg_color(s_ball, lv_color_hex(0xE6E9EF), 0);
    lv_obj_set_style_bg_opa(s_ball, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ball, 0, 0);
    lv_obj_set_style_radius(s_ball, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_ball, 0, 0);

    /* 球拍 */
    s_pad = lv_obj_create(s_field);
    lv_obj_remove_flag(s_pad, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_pad, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_pad, PAD_R * 2, PAD_R * 2);
    lv_obj_set_style_bg_color(s_pad, UI_COLOR_PRIMARY, 0);
    lv_obj_set_style_bg_opa(s_pad, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_pad, 2, 0);
    lv_obj_set_style_border_color(s_pad, lv_color_hex(0x7FD4FF), 0);
    lv_obj_set_style_radius(s_pad, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_pad, 0, 0);

    game_reset();

    /* 定时器与页面同生命周期：页面销毁时自动删 */
    ui_game_timer_bind(root, lv_timer_create(soccer_tick, TICK_MS, NULL));
    lv_obj_add_event_cb(root, soccer_del_cb, LV_EVENT_DELETE, NULL);
}
