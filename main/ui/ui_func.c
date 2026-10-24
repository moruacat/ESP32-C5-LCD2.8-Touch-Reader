#include "ui_func.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "ui_dialog.h"
#include "app_navigator.h"
#include "srv_config.h"

#include <string.h>

#define CARD_W      148
#define CARD_H      94
#define X0          8
#define Y0          UI_CONTENT_Y
#define GAP         8

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

void ui_func_create(lv_obj_t *root)
{
    ui_titlebar_create(root, "功能", app_navigator_back_smart_cb, NULL);

    /* 第一行：书架 | 音乐 */
    ui_card_create(root, X0, Y0, CARD_W, CARD_H,
                   &ic_bookshelf, "书架", 0x8B5A2B, on_bookshelf, NULL);
    ui_card_create(root, X0 + CARD_W + GAP, Y0, CARD_W, CARD_H,
                   &ic_music, "音乐", 0x2E8B57, on_music, NULL);

    /* 第二行：游戏 | 电台 | 照片 | 录音机
     * 四张窄卡排满一行：左边两张对齐「书架」，右边两张对齐「音乐」。 */
    const lv_coord_t row_y  = Y0 + CARD_H + GAP;
    const lv_coord_t row_x  = X0 + CARD_W + GAP;
    const lv_coord_t half_w = (CARD_W - GAP) / 2;
    const lv_coord_t step   = half_w + GAP;

    ui_card_create(root, X0, row_y, half_w, CARD_H,
                   &ic_game, "游戏", 0xC06A2B, on_games, NULL);
    ui_card_create(root, X0 + step, row_y, half_w, CARD_H,
                   &ic_radio, "电台", 0x1585C0, on_radio, NULL);

    ui_card_create(root, row_x, row_y, half_w, CARD_H,
                   &ic_image, "照片", 0x3B9EFF, on_photos, NULL);
    ui_card_create(root, row_x + step, row_y, half_w, CARD_H,
                   &ic_record, "录音机", 0xE5484D, on_recorder, NULL);
}
