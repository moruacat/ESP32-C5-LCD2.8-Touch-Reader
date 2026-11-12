#include "ui_func.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "ui_dialog.h"
#include "app_navigator.h"
#include "srv_config.h"

#include <string.h>

#define X0          8
#define Y0          UI_CONTENT_Y
#define GAP         8

/* 三列等分（每列 96 宽），布局按草图的大致占比：
 *
 *   ┌────────── 书架（跨 2 列）──────────┐ ┌────┐
 *   ├───────────┬───────────┤           │ │    │
 *   │   游戏    │   电台    │           │ │音乐│
 *   ├───────────┼───────────┼───────────┤ │    │
 *   │   照片    │  录音机   │    AI     │ │    │
 *   └───────────┴───────────┴───────────┘ └────┘
 *
 * 音乐跨前两行，第三行三格均分。 */
#define COL_W       96
#define X0          8
#define Y0          UI_CONTENT_Y
#define GAP         8
#define COL_STEP    (COL_W + GAP)
#define ROW1_H      64
#define ROW2_H      56
#define ROW3_H      56
#define ROW2_Y      (Y0 + ROW1_H + GAP)
#define ROW3_Y      (Y0 + ROW1_H + GAP + ROW2_H + GAP)

/* ===================== 照片入口（带密码校验） ===================== */

/**
 * @brief 密码输入回调：对上了才进照片页
 */
static void on_pass_done(const char *text, void *arg)
{
    (void)arg;

    const char *pass = srv_config()->photo_pass;
    if (strcmp(text, pass) != 0) {
        ui_alert_show("密码错误", UI_COLOR_TEMP, "再试一次", "知道了",
                      NULL, NULL, 1800);
        return;
    }

    app_navigator_switch(PAGE_PHOTOS, true);
}

static void enter_photos(void)
{
    /* 没设密码就直接进（空目录时页面自己会提示） */
    if (srv_config()->photo_pass[0] == '\0') {
        app_navigator_switch(PAGE_PHOTOS, true);
        return;
    }

    ui_pass_input_show("输入照片密码", on_pass_done, NULL);
}

/* ===================== 各卡片回调 ===================== */

static void on_bookshelf(lv_event_t *e)
{
    (void)e;
    app_navigator_switch(PAGE_BOOKSHELF, true);
}

static void on_music(lv_event_t *e)
{
    (void)e;
    app_navigator_switch(PAGE_MUSIC, true);
}

static void on_games(lv_event_t *e)
{
    (void)e;
    app_navigator_switch(PAGE_GAMES, true);
}

static void on_radio(lv_event_t *e)
{
    (void)e;
    app_navigator_switch(PAGE_RADIO, true);
}

static void on_recorder(lv_event_t *e)
{
    (void)e;
    app_navigator_switch(PAGE_RECORDER, true);
}

static void on_photos(lv_event_t *e)
{
    (void)e;
    enter_photos();
}

static void on_ai(lv_event_t *e)
{
    (void)e;
    app_navigator_switch(PAGE_AI, true);
}

void ui_func_create(lv_obj_t *root)
{
    ui_titlebar_create(root, "功能", app_navigator_back_smart_cb, NULL);

    /* 第一行：书架跨两列 */
    ui_card_create(root, X0, Y0, COL_W * 2 + GAP, ROW1_H,
                   &ic_bookshelf, "书架", 0x8B5A2B, on_bookshelf, NULL);

    /* 第二行：游戏 | 电台 */
    ui_card_create(root, X0, ROW2_Y, COL_W, ROW2_H,
                   &ic_game, "游戏", 0xC06A2B, on_games, NULL);
    ui_card_create(root, X0 + COL_STEP, ROW2_Y, COL_W, ROW2_H,
                   &ic_radio, "电台", 0x1585C0, on_radio, NULL);

    /* 第三行：照片 | 录音机 | AI */
    ui_card_create(root, X0, ROW3_Y, COL_W, ROW3_H,
                   &ic_image, "照片", 0x3B9EFF, on_photos, NULL);
    ui_card_create(root, X0 + COL_STEP, ROW3_Y, COL_W, ROW3_H,
                   &ic_record, "录音机", 0xE5484D, on_recorder, NULL);
    ui_card_create(root, X0 + COL_STEP * 2, ROW3_Y, COL_W, ROW3_H,
                   &ic_ai, "AI", 0x7C4DFF, on_ai, NULL);

    /* 右侧一列：音乐，跨前两行 */
    ui_card_create(root, X0 + COL_STEP * 2, Y0, COL_W, ROW1_H + GAP + ROW2_H,
                   &ic_music, "音乐", 0x2E8B57, on_music, NULL);
}
