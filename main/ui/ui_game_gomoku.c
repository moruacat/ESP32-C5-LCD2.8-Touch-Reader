#include "ui_game_gomoku.h"
#include "ui_common.h"
#include "ui_games.h"
#include "ui_dialog.h"
#include "app_navigator.h"

#include <stdio.h>
#include <string.h>

/* 棋盘：15x15 交叉点，格 13px，四边留 4px 边距 → 190x190 */
#define N           15
#define GRID        13
#define PAD         4
#define BD_SIZE     (PAD * 2 + (N - 1) * GRID)          /* 190 */
#define BD_X        8
#define BD_Y        UI_CONTENT_Y

#define STONE       11                                   /*!< 棋子直径 */
#define IND         19                                   /*!< 落子指示器直径（比棋子大一圈） */

#define SIDE_X      (BD_X + BD_SIZE + 8)                 /* 206 */
#define SIDE_W      (UI_SCREEN_W - SIDE_X - 8)           /* 106 */

#define BTN_W       96
#define BTN_H       46
#define BTN_X       (SIDE_X + (SIDE_W - BTN_W) / 2)
#define BTN_Y       94

#define PLY_BLACK   1
#define PLY_WHITE   2

static lv_obj_t *s_root;
static lv_obj_t *s_board;
static lv_obj_t *s_ind;           /*!< 落子指示器（预览会落在哪） */
static lv_obj_t *s_info_lb;       /*!< 右侧提示：轮到谁 / 胜负 */
static lv_obj_t *s_dot;           /*!< 右侧当前方色块 */
static lv_obj_t *s_moves_lb;

static uint8_t   s_grid[N][N];    /*!< s_grid[y][x]，0 空 / PLY_xxx */
static lv_obj_t *s_stones[N * N];
static int       s_stone_cnt;
static int       s_turn;
static int       s_moves;
static bool      s_over;

static int       s_ix, s_iy;      /*!< 指示器所在交叉点 */

static void game_reset(void);

static void restart_game(void);

/* ===================== 规则 ===================== */

static bool check_win(int x, int y, uint8_t who)
{
    static const int dir[4][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};

    for (int d = 0; d < 4; d++) {
        int cnt = 1;

        for (int s = 1; s < 5; s++) {
            int nx = x + dir[d][0] * s;
            int ny = y + dir[d][1] * s;
            if (nx < 0 || nx >= N || ny < 0 || ny >= N || s_grid[ny][nx] != who) {
                break;
            }
            cnt++;
        }
        for (int s = 1; s < 5; s++) {
            int nx = x - dir[d][0] * s;
            int ny = y - dir[d][1] * s;
            if (nx < 0 || nx >= N || ny < 0 || ny >= N || s_grid[ny][nx] != who) {
                break;
            }
            cnt++;
        }
        if (cnt >= 5) {
            return true;
        }
    }
    return false;
}

/* ===================== 指示器 ===================== */

static void ind_set(int x, int y)
{
    s_ix = x;
    s_iy = y;
    lv_obj_set_pos(s_ind, PAD + x * GRID - IND / 2, PAD + y * GRID - IND / 2);
}

/* 指示器的颜色跟着当前落子方：黑子时深色圈、白子时浅色圈，
 * 半透明填充让人一眼看出"这是预览，还没落下" */
static void ind_update_color(void)
{
    const bool black = (s_turn == PLY_BLACK);
    const uint32_t c = black ? 0x14181D : 0xE6E9EF;

    lv_obj_set_style_border_color(s_ind, lv_color_hex(c), 0);
    lv_obj_set_style_bg_color(s_ind, lv_color_hex(c), 0);
    lv_obj_set_style_bg_opa(s_ind, LV_OPA_40, 0);
}

/* 落子后指示器还停在刚占掉的那一点上，把它挪到最近的空位，
 * 省得玩家每次都要自己移开 */
static void ind_goto_empty_near(int cx, int cy)
{
    int best = -1;
    int best_d = 0x7FFFFFFF;

    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            if (s_grid[y][x] != 0) {
                continue;
            }
            int d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
            if (d < best_d) {
                best_d = d;
                best = y * N + x;
            }
        }
    }

    if (best >= 0) {
        ind_set(best % N, best / N);
    }
}

/* ===================== 右侧信息 ===================== */

static void update_info(void)
{
    char buf[32];

    if (s_over) {
        return;                    /* 结束时文案由 end_game 定 */
    }

    lv_label_set_text(s_info_lb, (s_turn == PLY_BLACK) ? "黑方落子" : "白方落子");
    lv_obj_set_style_bg_color(s_dot, (s_turn == PLY_BLACK) ? lv_color_hex(0x14181D)
                                                          : lv_color_hex(0xE6E9EF), 0);

    snprintf(buf, sizeof(buf), "第 %d 手", s_moves + 1);
    lv_label_set_text(s_moves_lb, buf);

    ind_update_color();
}

static void end_game(const char *title, lv_color_t color, const char *info, const char *note)
{
    s_over = true;
    lv_label_set_text(s_info_lb, info);
    lv_obj_set_style_text_color(s_info_lb, color, 0);
    lv_obj_add_flag(s_ind, LV_OBJ_FLAG_HIDDEN);     /* 结束了就别再预览落点 */
    ui_alert_show(title, color, note, "重新开始", ui_game_alert_restart_cb, NULL, 0);
}

static void place_stone(int x, int y, uint8_t who)
{
    lv_obj_t *st = lv_obj_create(s_board);
    lv_obj_remove_flag(st, LV_OBJ_FLAG_SCROLLABLE);
    /* 棋子不能可点击：命中测试会把按下事件判给最深层的可点击对象且不再上传，
     * 点在已有棋子上时棋盘就收不到事件了 */
    lv_obj_remove_flag(st, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(st, STONE, STONE);
    lv_obj_set_pos(st, PAD + x * GRID - STONE / 2, PAD + y * GRID - STONE / 2);
    lv_obj_set_style_bg_opa(st, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(st, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(st, 0, 0);

    if (who == PLY_BLACK) {
        /* 深色主题下纯黑棋子会和棋盘糊在一起，加一圈浅描边 */
        lv_obj_set_style_bg_color(st, lv_color_hex(0x14181D), 0);
        lv_obj_set_style_border_width(st, 1, 0);
        lv_obj_set_style_border_color(st, lv_color_hex(0x606A78), 0);
    } else {
        lv_obj_set_style_bg_color(st, lv_color_hex(0xE6E9EF), 0);
        lv_obj_set_style_border_width(st, 0, 0);
    }

    s_stones[s_stone_cnt++] = st;
}

static void game_reset(void)
{
    for (int i = 0; i < s_stone_cnt; i++) {
        if (s_stones[i] != NULL && lv_obj_is_valid(s_stones[i])) {
            lv_obj_delete(s_stones[i]);
        }
        s_stones[i] = NULL;
    }
    s_stone_cnt = 0;

    memset(s_grid, 0, sizeof(s_grid));
    s_turn = PLY_BLACK;
    s_moves = 0;
    s_over  = false;

    lv_obj_set_style_text_color(s_info_lb, UI_COLOR_TEXT_DIM, 0);
    lv_obj_remove_flag(s_ind, LV_OBJ_FLAG_HIDDEN);

    /* 默认落在天元，开局直接点「落子」就能下第一手 */
    ind_set(N / 2, N / 2);
    update_info();
}

static void restart_game(void)
{
    if (!ui_game_page_active()) {
        return;
    }
    game_reset();
}

/* ===================== 触摸 ===================== */

/* 屏幕坐标 → 最近的交叉点；不在棋盘里返回 false */
static bool point_to_cell(lv_point_t *p, int *out_x, int *out_y)
{
    int rel_x = p->x - BD_X - PAD;
    int rel_y = p->y - BD_Y - PAD;

    /* 四舍五入到最近的那条线。
     * 这里不能直接写 (rel + GRID/2) / GRID —— 整数除法是向零截断，
     * rel 为负时（点在棋盘左/上边缘之外一点点）会被算成第 0 列，
     * 于是界外的一指也会落到边线上。负数单独处理成"向下取整"。 */
    int x = (rel_x >= 0) ? (rel_x + GRID / 2) / GRID
                         : -((-rel_x + GRID / 2) / GRID);
    int y = (rel_y >= 0) ? (rel_y + GRID / 2) / GRID
                         : -((-rel_y + GRID / 2) / GRID);

    if (x < 0 || x >= N || y < 0 || y >= N) {
        return false;
    }

    *out_x = x;
    *out_y = y;
    return true;
}

/* 按下和拖动都移动指示器：手指划到哪预览就跟到哪，抬手前不会落子 */
static void on_board_move(lv_event_t *e)
{
    if (s_over || s_board == NULL) {
        return;
    }

    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    int x, y;
    if (!point_to_cell(&p, &x, &y)) {
        return;
    }
    if (s_grid[y][x] != 0) {
        return;                    /* 那一点已经有子，指示器不过去 */
    }
    if (x == s_ix && y == s_iy) {
        return;
    }

    ind_set(x, y);
}

/* 「落子」按钮：确认把指示器那一点下下来 */
static void on_place(lv_event_t *e)
{
    (void)e;

    if (s_over || s_board == NULL || s_root == NULL) {
        return;
    }
    if (s_grid[s_iy][s_ix] != 0) {
        return;                    /* 该点已被占（理论上不会） */
    }

    const int x = s_ix;
    const int y = s_iy;

    s_grid[y][x] = (uint8_t)s_turn;
    place_stone(x, y, (uint8_t)s_turn);
    s_moves++;

    if (check_win(x, y, (uint8_t)s_turn)) {
        char note[32];
        snprintf(note, sizeof(note), "共 %d 手", s_moves);

        const bool black = (s_turn == PLY_BLACK);
        end_game(black ? "黑方胜利" : "白方胜利",
                 black ? UI_COLOR_TEXT : UI_COLOR_PRIMARY,
                 black ? "黑方胜" : "白方胜", note);
        return;
    }

    if (s_moves >= N * N) {
        end_game("和棋", UI_COLOR_WARN, "和棋", "棋盘满了");
        return;
    }

    s_turn = (s_turn == PLY_BLACK) ? PLY_WHITE : PLY_BLACK;

    ind_goto_empty_near(x, y);
    update_info();
}

/* ===================== 页面 ===================== */

static void gomoku_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    if (lv_event_get_target(e) != s_root) {
        return;
    }
    s_root     = NULL;
    s_board    = NULL;
    s_ind      = NULL;
    s_info_lb  = NULL;
    s_dot      = NULL;
    s_moves_lb = NULL;
    s_stone_cnt = 0;
    for (int i = 0; i < N * N; i++) {
        s_stones[i] = NULL;
    }
}

void ui_game_gomoku_create(lv_obj_t *root)
{
    s_root = root;
    memset(s_stones, 0, sizeof(s_stones));
    s_stone_cnt = 0;
    ui_game_page_enter(root, PAGE_GAME_GOMOKU, restart_game);

    ui_game_bar_create(root, "五子棋");

    s_board = lv_obj_create(root);
    lv_obj_remove_flag(s_board, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_board, BD_SIZE, BD_SIZE);
    lv_obj_set_pos(s_board, BD_X, BD_Y);
    lv_obj_set_style_bg_color(s_board, lv_color_hex(0x1B222B), 0);
    lv_obj_set_style_bg_opa(s_board, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_board, 0, 0);
    lv_obj_set_style_radius(s_board, 4, 0);
    lv_obj_set_style_pad_all(s_board, 0, 0);
    lv_obj_add_flag(s_board, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_board, on_board_move, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_board, on_board_move, LV_EVENT_PRESSING, NULL);

    /* 棋盘线：15 横 + 15 竖，各一条 1px 的细条对象。
     *
     * 必须摘掉 CLICKABLE —— lv_obj_create() 默认带这个标志，而 LVGL 的命中
     * 测试会把按下事件判给最深层的可点击对象、不再往上传。竖线正好各占一个
     * 像素列，点在线上时棋盘收不到事件，指示器就不动。 */
    for (int i = 0; i < N; i++) {
        lv_obj_t *h = lv_obj_create(s_board);
        lv_obj_remove_flag(h, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(h, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(h, (N - 1) * GRID + 1, 1);
        lv_obj_set_pos(h, PAD, PAD + i * GRID);
        lv_obj_set_style_bg_color(h, lv_color_hex(0x39434E), 0);
        lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(h, 0, 0);
        lv_obj_set_style_radius(h, 0, 0);
        lv_obj_set_style_pad_all(h, 0, 0);

        lv_obj_t *v = lv_obj_create(s_board);
        lv_obj_remove_flag(v, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(v, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(v, 1, (N - 1) * GRID + 1);
        lv_obj_set_pos(v, PAD + i * GRID, PAD);
        lv_obj_set_style_bg_color(v, lv_color_hex(0x39434E), 0);
        lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(v, 0, 0);
        lv_obj_set_style_radius(v, 0, 0);
        lv_obj_set_style_pad_all(v, 0, 0);
    }

    /* 落子指示器。建在棋盘线之后 → 画在它们上面；
     * 同样要摘掉 CLICKABLE，不然按住指示器本身时棋盘收不到拖动事件。 */
    s_ind = lv_obj_create(s_board);
    lv_obj_remove_flag(s_ind, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_ind, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_ind, IND, IND);
    lv_obj_set_style_border_width(s_ind, 2, 0);
    lv_obj_set_style_radius(s_ind, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_ind, 0, 0);

    /* ===== 右侧：当前方 + 落子按钮 + 手数 ===== */
    s_info_lb = lv_label_create(root);
    lv_obj_set_style_text_color(s_info_lb, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_info_lb, &font_ui_14, 0);
    lv_obj_set_style_text_align(s_info_lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_info_lb, SIDE_W);
    lv_label_set_text(s_info_lb, "黑方落子");
    lv_obj_set_pos(s_info_lb, SIDE_X, 40);

    s_dot = lv_obj_create(root);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_dot, 18, 18);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(0x14181D), 0);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_dot, 1, 0);
    lv_obj_set_style_border_color(s_dot, lv_color_hex(0x606A78), 0);
    lv_obj_set_style_pad_all(s_dot, 0, 0);
    lv_obj_set_pos(s_dot, SIDE_X + SIDE_W / 2 - 9, 64);

    lv_obj_t *btn = lv_button_create(root);
    lv_obj_set_size(btn, BTN_W, BTN_H);
    lv_obj_set_pos(btn, BTN_X, BTN_Y);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, UI_COLOR_PRIMARY, 0);
    ui_add_click_feedback(btn);
    lv_obj_add_event_cb(btn, on_place, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *lb = lv_label_create(btn);
        lv_obj_set_style_text_color(lb, lv_color_white(), 0);
        lv_obj_set_style_text_font(lb, &font_ui_14, 0);
        lv_label_set_text(lb, "落子");
        lv_obj_center(lb);
    }

    s_moves_lb = lv_label_create(root);
    lv_obj_set_style_text_color(s_moves_lb, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_moves_lb, &font_ui_14, 0);
    lv_obj_set_style_text_align(s_moves_lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_moves_lb, SIDE_W);
    lv_label_set_text(s_moves_lb, "第 1 手");
    lv_obj_set_pos(s_moves_lb, SIDE_X, 154);

    {
        lv_obj_t *tip = lv_label_create(root);
        lv_obj_set_style_text_color(tip, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_font(tip, &font_ui_14, 0);
        lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(tip, SIDE_W);
        lv_label_set_text(tip, "点棋盘选位置\n再按落子");
        lv_obj_set_pos(tip, SIDE_X, 184);
    }

    game_reset();

    lv_obj_add_event_cb(root, gomoku_del_cb, LV_EVENT_DELETE, NULL);
}
