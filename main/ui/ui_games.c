#include "ui_games.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "ui_dialog.h"
#include "app_navigator.h"
#include "srv_player.h"

#include <stdlib.h>

/* 卡片：2 列 3 行铺满标题栏以下（每张 148x62） */
#define CARD_W      148
#define CARD_H      62
#define X0          8
#define Y0          UI_CONTENT_Y
#define GAP         8

/*! 手指位移超过这个像素才算一次滑动 */
#define SWIPE_MIN   18

/* 当前游戏页。同一时刻只会存在一个游戏页，所以状态只留一份。 */
static lv_obj_t            *s_page_root  = NULL;
static app_page_id_t        s_page_id    = PAGE_MAX;
static ui_game_restart_cb_t s_restart_cb = NULL;

/* 滑动手势状态 */
static lv_point_t         s_swipe_start;
static ui_game_swipe_cb_t s_swipe_cb  = NULL;
static void              *s_swipe_arg = NULL;

/* ===================== 游戏页公共入口 ===================== */

static void game_page_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    /* 切页是异步删除：手快「游戏 A → 别的 → 游戏 A」时新页面已经建好，
     * 只有目标就是当前记录的根对象时才清空，否则清掉的是新页面的记录 */
    if (lv_event_get_target(e) != s_page_root) {
        return;
    }
    s_page_root  = NULL;
    s_page_id    = PAGE_MAX;
    s_restart_cb = NULL;
}

void ui_game_page_enter(lv_obj_t *root, app_page_id_t page,
                        ui_game_restart_cb_t on_restart)
{
    s_page_root  = root;
    s_page_id    = page;
    s_restart_cb = on_restart;
    lv_obj_add_event_cb(root, game_page_del_cb, LV_EVENT_DELETE, NULL);

    /* 带着后台音乐进游戏会和解码任务抢 CPU，主动收掉 */
    if (srv_player_state() != SRV_PLAYER_IDLE) {
        (void)srv_player_stop();
    }
}

bool ui_game_page_active(void)
{
    return s_page_root != NULL && app_navigator_current() == s_page_id;
}

void ui_game_alert_restart_cb(void *arg)
{
    (void)arg;
    if (ui_game_page_active() && s_restart_cb != NULL) {
        s_restart_cb();
    }
}

/* ===================== 滑动手势 ===================== */

static void swipe_pressed_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }
    lv_indev_get_point(indev, &s_swipe_start);
}

static void swipe_released_cb(lv_event_t *e)
{
    (void)e;
    if (s_swipe_cb == NULL) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    int dx = p.x - s_swipe_start.x;
    int dy = p.y - s_swipe_start.y;
    if (abs(dx) < SWIPE_MIN && abs(dy) < SWIPE_MIN) {
        return;                     /* 轻点不算 */
    }

    int dir = (abs(dx) > abs(dy)) ? ((dx > 0) ? 3 : 2)
                                  : ((dy > 0) ? 1 : 0);
    s_swipe_cb(dir, s_swipe_arg);
}

void ui_game_swipe_enable(lv_obj_t *root, ui_game_swipe_cb_t cb, void *arg)
{
    s_swipe_cb  = cb;
    s_swipe_arg = arg;

    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root, swipe_pressed_cb,  LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(root, swipe_released_cb, LV_EVENT_RELEASED, NULL);
}

/* ===================== 定时器生命周期 ===================== */

static void timer_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    /* 句柄跟着事件一起来，必定属于这个正在销毁的页面，直接删 */
    lv_timer_t *t = (lv_timer_t *)lv_event_get_user_data(e);
    if (t != NULL) {
        lv_timer_delete(t);
    }
}

void ui_game_timer_bind(lv_obj_t *root, lv_timer_t *tmr)
{
    lv_obj_add_event_cb(root, timer_del_cb, LV_EVENT_DELETE, tmr);
}

/* ===================== 游戏页公共标题栏 ===================== */

static void game_menu_cb(int idx, void *arg)
{
    (void)arg;
    if (idx == 0) {
        if (s_restart_cb != NULL) {
            s_restart_cb();         /* 各游戏的 restart 自己会确认还在本页 */
        }
    } else if (idx == 1) {
        app_navigator_back();
    }
}

static void on_game_menu(lv_event_t *e)
{
    (void)e;
    static const ui_menu_item_t items[] = {
        {"重新开始", &ic_reset},
        {"退出游戏", &ic_arrow_left},
    };
    ui_menu_show(NULL, items, 2, game_menu_cb, NULL);
}

lv_obj_t *ui_game_bar_create(lv_obj_t *root, const char *title)
{
    /* 复用公共标题栏骨架：右侧不放时钟，留给下面的菜单键。
     * 顺带得到标题限宽（超长收省略号），这是原来手写版漏掉的。
     * 游戏标题不滚动：棋盘/球场的视野要稳。 */
    lv_obj_t *bar = ui_titlebar_create_plain(root, title, app_navigator_back_smart_cb, NULL);
    ui_titlebar_set_scroll(bar, false);

    /* 右：菜单键（重新开始 / 退出游戏） */
    lv_obj_t *menu = lv_button_create(bar);
    lv_obj_set_size(menu, 26, 22);
    lv_obj_align(menu, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_radius(menu, 6, 0);
    lv_obj_set_style_bg_color(menu, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_shadow_width(menu, 0, 0);
    ui_add_click_feedback(menu);
    lv_obj_add_event_cb(menu, on_game_menu, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ic = lv_image_create(menu);
    lv_image_set_src(ic, &ic_menu);
    lv_obj_set_style_image_recolor(ic, UI_COLOR_TEXT, 0);
    lv_obj_set_style_radius(ic, 0, 0);
    lv_obj_center(ic);

    return bar;
}

/* ===================== 游戏列表页 ===================== */

static void open_2048(lv_event_t *e)
{
    (void)e;
    app_navigator_switch(PAGE_GAME_2048, true);
}

static void open_snake(lv_event_t *e)
{
    (void)e;
    app_navigator_switch(PAGE_GAME_SNAKE, true);
}

static void open_gomoku(lv_event_t *e)
{
    (void)e;
    app_navigator_switch(PAGE_GAME_GOMOKU, true);
}

static void open_paint(lv_event_t *e)
{
    (void)e;
    app_navigator_switch(PAGE_GAME_PAINT, true);
}

static void open_ball(lv_event_t *e)
{
    (void)e;
    app_navigator_switch(PAGE_GAME_BALL, true);
}

static void open_soccer(lv_event_t *e)
{
    (void)e;
    app_navigator_switch(PAGE_GAME_SOCCER, true);
}

/* 标题 + 说明两行文字的卡片。和 ui_card_create 的区别是它带副标题、
 * 不摆图标，位置也不同，所以没有合并。 */
static lv_obj_t *make_game_card(lv_obj_t *root, lv_coord_t x, lv_coord_t y,
                                const char *name, const char *note,
                                lv_event_cb_t cb)
{
    lv_obj_t *card = lv_obj_create(root);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(card, CARD_W, CARD_H);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_color(card, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    ui_add_click_feedback(card);
    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lb = lv_label_create(card);
    lv_obj_set_style_text_font(lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(lb, UI_COLOR_TEXT, 0);
    lv_label_set_text(lb, name);
    lv_obj_align(lb, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *nt = lv_label_create(card);
    lv_obj_set_style_text_font(nt, &font_ui_14, 0);
    lv_obj_set_style_text_color(nt, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(nt, note);
    lv_obj_align(nt, LV_ALIGN_TOP_MID, 0, 34);

    return card;
}

void ui_games_create(lv_obj_t *root)
{
    ui_titlebar_create(root, "游戏", app_navigator_back_smart_cb, NULL);

    static const struct {
        const char   *name;
        const char   *note;
        lv_event_cb_t cb;
    } item[] = {
        {"2048",   "数字合并", open_2048},
        {"贪吃蛇", "经典玩法", open_snake},
        {"五子棋", "双人对战", open_gomoku},
        {"画板",   "随手涂鸦", open_paint},
        {"重力球", "倾斜机身", open_ball},
        {"足球",   "拨球进门", open_soccer},
    };

    const int n = (int)(sizeof(item) / sizeof(item[0]));
    for (int i = 0; i < n; i++) {
        lv_coord_t x = X0 + (i % 2) * (CARD_W + GAP);
        lv_coord_t y = Y0 + (i / 2) * (CARD_H + GAP);
        make_game_card(root, x, y, item[i].name, item[i].note, item[i].cb);
    }
}