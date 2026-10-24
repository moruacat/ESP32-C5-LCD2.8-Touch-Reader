#include "ui_game_2048.h"
#include "ui_common.h"
#include "ui_games.h"
#include "ui_dialog.h"
#include "app_navigator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(lv_font_montserrat_24);

/* 棋盘：4x4，格 44 + 缝 4 → 188x188，放在左下，右侧留一栏显示得分 */
#define BD_X        8
#define BD_Y        UI_CONTENT_Y
#define CELL        44
#define CELL_GAP    4
#define BD_SIZE     (CELL * 4 + CELL_GAP * 3)          /* 188 */

#define SIDE_X      (BD_X + BD_SIZE + 8)               /* 204 */
#define SIDE_W      (UI_SCREEN_W - SIDE_X - 8)         /* 108 */

static lv_obj_t *s_root;
static lv_obj_t *s_cells[4][4];
static lv_obj_t *s_cell_lb[4][4];
static lv_obj_t *s_score_lb;

static uint16_t  s_grid[4][4];
static int       s_score;
static bool      s_over;          /*!< 无路可走，等玩家点重新开始 */

static void render(void);

/* ===================== 规则 ===================== */

/* 数字对应的底色：越大越暖，2048 以上统一金色 */
static uint32_t tile_color(uint16_t v)
{
    switch (v) {
    case 0:    return 0x1B222B;
    case 2:    return 0x2C3742;
    case 4:    return 0x38495A;
    case 8:    return 0x2E6E8C;
    case 16:   return 0x1585C0;
    case 32:   return 0x1D7FA6;
    case 64:   return 0xB86A2B;
    case 128:  return 0xCE8432;
    case 256:  return 0xD8A030;
    case 512:  return 0xDDBE4D;
    case 1024: return 0xD94A4F;
    default:   return 0xE8C84D;
    }
}

/* 压紧一行并合并，返回本次得分；line[0] 是最靠近目标边的那一格 */
static int slide_line(uint16_t *line)
{
    uint16_t tmp[4] = {0, 0, 0, 0};
    int      n      = 0;
    int      gain   = 0;

    for (int i = 0; i < 4; i++) {
        if (line[i] == 0) {
            continue;
        }
        if (n > 0 && tmp[n - 1] == line[i]) {
            tmp[n - 1] = (uint16_t)(tmp[n - 1] * 2);   /* 合并后不再参与合并 */
            gain += tmp[n - 1];
        } else {
            tmp[n++] = line[i];
        }
    }

    memcpy(line, tmp, sizeof(tmp));
    return gain;
}

/* dir: 0=上 1=下 2=左 3=右；返回 false 表示这一步没有任何变化 */
static bool move_dir(int dir)
{
    uint16_t before[4][4];
    memcpy(before, s_grid, sizeof(before));

    int gain = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t line[4];

        for (int j = 0; j < 4; j++) {
            switch (dir) {
            case 0:  line[j] = s_grid[j][i];     break;   /* 上：该列自上而下 */
            case 1:  line[j] = s_grid[3 - j][i]; break;   /* 下：该列自下而上 */
            case 2:  line[j] = s_grid[i][j];     break;   /* 左：该行自左而右 */
            default: line[j] = s_grid[i][3 - j]; break;   /* 右：该行自右而左 */
            }
        }

        gain += slide_line(line);

        for (int j = 0; j < 4; j++) {
            switch (dir) {
            case 0:  s_grid[j][i]     = line[j]; break;
            case 1:  s_grid[3 - j][i] = line[j]; break;
            case 2:  s_grid[i][j]     = line[j]; break;
            default: s_grid[i][3 - j] = line[j]; break;
            }
        }
    }

    if (memcmp(before, s_grid, sizeof(before)) == 0) {
        return false;
    }
    s_score += gain;
    return true;
}

static void spawn_tile(void)
{
    int row[16];
    int col[16];
    int n = 0;

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (s_grid[r][c] == 0) {
                row[n] = r;
                col[n] = c;
                n++;
            }
        }
    }
    if (n == 0) {
        return;
    }

    int k = rand() % n;
    s_grid[row[k]][col[k]] = (rand() % 10 == 0) ? 4 : 2;   /* 约 1/10 出 4 */
}

/* 还有空格、或存在相邻同数就还能走 */
static bool can_move(void)
{
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (s_grid[r][c] == 0) {
                return true;
            }
            if (c + 1 < 4 && s_grid[r][c] == s_grid[r][c + 1]) {
                return true;
            }
            if (r + 1 < 4 && s_grid[r][c] == s_grid[r + 1][c]) {
                return true;
            }
        }
    }
    return false;
}

/* ===================== 渲染 ===================== */

static void render(void)
{
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            uint16_t v = s_grid[r][c];

            lv_obj_set_style_bg_color(s_cells[r][c], lv_color_hex(tile_color(v)), 0);

            lv_obj_t *lb = s_cell_lb[r][c];
            if (v == 0) {
                lv_obj_add_flag(lb, LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            lv_obj_remove_flag(lb, LV_OBJ_FLAG_HIDDEN);

            char buf[8];
            snprintf(buf, sizeof(buf), "%u", (unsigned)v);
            lv_label_set_text(lb, buf);

            /* 两位数以内用大号字，三位以上换小号，否则会顶出格子 */
            lv_obj_set_style_text_font(lb, (v < 100) ? &lv_font_montserrat_24
                                                     : &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(lb, (v <= 4) ? UI_COLOR_TEXT
                                                     : lv_color_white(), 0);
            lv_obj_center(lb);
        }
    }

    char sbuf[16];
    snprintf(sbuf, sizeof(sbuf), "%d", s_score);
    lv_label_set_text(s_score_lb, sbuf);
}

static void new_game(void)
{
    memset(s_grid, 0, sizeof(s_grid));
    s_score = 0;
    s_over  = false;
    spawn_tile();
    spawn_tile();
    render();
}

static void restart_game(void)
{
    /* 菜单/弹层的回调可能晚于页面切换送达，这里再确认一次当前就是本页 */
    if (!ui_game_page_active()) {
        return;
    }
    new_game();
}

/* ===================== 触摸 ===================== */

/* 方向由 ui_games 统一判定：0=上 1=下 2=左 3=右 */
static void on_swipe(int dir, void *arg)
{
    (void)arg;
    if (s_over) {
        return;
    }

    if (!move_dir(dir)) {
        return;                      /* 这个方向动不了，不补新块 */
    }

    spawn_tile();
    render();

    if (!can_move()) {
        s_over = true;
        char note[32];
        snprintf(note, sizeof(note), "得分 %d", s_score);
        ui_alert_show("无路可走了", UI_COLOR_TEMP, note, "重新开始",
                      ui_game_alert_restart_cb, NULL, 0);
    }
}

/* ===================== 页面 ===================== */

static void game_2048_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    /* 切页是异步删除，手快「2048→别的→2048」时新页面已经建好，
     * 只有目标就是当前这份根对象时才清空 */
    if (lv_event_get_target(e) != s_root) {
        return;
    }
    s_root     = NULL;
    s_score_lb = NULL;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            s_cells[r][c]   = NULL;
            s_cell_lb[r][c] = NULL;
        }
    }
}

void ui_game_2048_create(lv_obj_t *root)
{
    s_root = root;
    ui_game_page_enter(root, PAGE_GAME_2048, restart_game);

    ui_game_bar_create(root, "2048");

    /* 棋盘底板 + 16 个格子。
     *
     * 这些对象都要摘掉 CLICKABLE：lv_obj_create() 默认带这个标志，而 LVGL 的
     * 命中测试是「从最后一个子对象往前找，谁先命中且可点击就归谁」，命中后
     * **不再往父对象传**。不摘的话按下会落在某个格子上，挂在 root 上的
     * PRESSED/RELEASED 永远收不到 —— 表现就是滑动完全没反应。
     * 摘掉之后按下会一路落到 root，滑出棋盘、滑到标题栏都照样算数。 */
    lv_obj_t *board = lv_obj_create(root);
    lv_obj_remove_flag(board, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(board, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(board, BD_SIZE, BD_SIZE);
    lv_obj_set_pos(board, BD_X, BD_Y);
    lv_obj_set_style_bg_color(board, lv_color_hex(0x141A21), 0);
    lv_obj_set_style_bg_opa(board, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(board, 0, 0);
    lv_obj_set_style_radius(board, 8, 0);
    lv_obj_set_style_pad_all(board, 0, 0);

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            lv_obj_t *cell = lv_obj_create(board);
            lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(cell, LV_OBJ_FLAG_CLICKABLE);   /* 别抢滑动 */
            lv_obj_set_size(cell, CELL, CELL);
            lv_obj_set_pos(cell, c * (CELL + CELL_GAP), r * (CELL + CELL_GAP));
            lv_obj_set_style_bg_color(cell, lv_color_hex(tile_color(0)), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(cell, 0, 0);
            lv_obj_set_style_radius(cell, 6, 0);
            lv_obj_set_style_pad_all(cell, 0, 0);

            lv_obj_t *lb = lv_label_create(cell);
            lv_obj_set_style_text_font(lb, &lv_font_montserrat_24, 0);
            lv_label_set_text(lb, "");
            lv_obj_center(lb);

            s_cells[r][c]   = cell;
            s_cell_lb[r][c] = lb;
        }
    }

    /* 右侧：得分 */
    lv_obj_t *cap = lv_label_create(root);
    lv_obj_set_style_text_color(cap, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(cap, &font_ui_14, 0);
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(cap, SIDE_W);
    lv_label_set_text(cap, "得分");
    lv_obj_set_pos(cap, SIDE_X, 40);

    s_score_lb = lv_label_create(root);
    lv_obj_set_style_text_color(s_score_lb, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_score_lb, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(s_score_lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_score_lb, SIDE_W);
    lv_label_set_text(s_score_lb, "0");
    lv_obj_set_pos(s_score_lb, SIDE_X, 60);

    lv_obj_t *tip = lv_label_create(root);
    lv_obj_set_style_text_color(tip, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(tip, &font_ui_14, 0);
    lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(tip, SIDE_W);
    lv_label_set_text(tip, "手指滑动\n合并同数");
    lv_obj_set_pos(tip, SIDE_X, 130);

    new_game();

    ui_game_swipe_enable(root, on_swipe, NULL);
    lv_obj_add_event_cb(root, game_2048_del_cb, LV_EVENT_DELETE, NULL);
}