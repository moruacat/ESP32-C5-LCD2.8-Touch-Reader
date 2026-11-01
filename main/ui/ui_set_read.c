#include "ui_set_pages.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "ui_dialog.h"
#include "app_navigator.h"
#include "srv_config.h"

#include <stdio.h>

/**
 * @brief 一个可加减的阅读参数
 *
 * 没有「字号」：字号需要多档汉字字体（目前只有 14px 一款），
 * 生成好字体再接进来，否则又是改了不起作用的假设置。
 */
typedef struct {
    const char           *name;
    const char           *key;
    const lv_image_dsc_t *icon;
    int                   step;     /*!< 每次加减的量 */
    int                   def;      /*!< 默认值（「恢复默认」用） */
} read_item_t;

static const read_item_t s_items[] = {
    {"字距", CFG_READ_LETTER_SP, &ic_letter_spacing,     1,  0},
    {"行距", CFG_READ_LINE_SP,   &ic_line_spacing,       1,  6},
    /* 段距一格 10：段间只能整行地留空，一格一档，点一下就能看出变化 */
    {"段距", CFG_READ_PARA_SP,   &ic_paragraph_spacing, 10,  0},
};

#define ITEM_COUNT  (sizeof(s_items) / sizeof(s_items[0]))

static lv_obj_t *s_val_lb[ITEM_COUNT];
static lv_obj_t *s_keep_lb;         /*!< 「阅读时不暗屏」的开关文字 */
static lv_obj_t *s_recent_lb;       /*!< 「最近阅读优先」的开关文字 */

static void read_refresh(void)
{
    for (int i = 0; i < (int)ITEM_COUNT; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", (int)srv_config_get_int(s_items[i].key, s_items[i].def));
        lv_label_set_text(s_val_lb[i], buf);
    }

    ui_row_switch_set(s_keep_lb, srv_config_get_int(CFG_READ_KEEP_BRIGHT, 0) != 0);
    ui_row_switch_set(s_recent_lb, srv_config_get_int(CFG_SHELF_RECENT, CFG_DEF_SHELF_RECENT) != 0);
}

/* ===================== 交互 ===================== */

static void on_step(lv_event_t *e)
{
    /* user_data = 索引 * 2 + 方向（0 = 减，1 = 加），
     * 省得为两个方向各写一个回调 */
    int packed = (int)(intptr_t)lv_event_get_user_data(e);
    int idx    = packed >> 1;
    int dir    = (packed & 1) ? 1 : -1;

    const read_item_t *it = &s_items[idx];
    int v = (int)srv_config_get_int(it->key, it->def) + dir * it->step;
    srv_config_set_int(it->key, v, true);      /* 越界会被 srv_config 夹住 */

    read_refresh();
}

static void on_keep_click(lv_event_t *e)
{
    (void)e;
    bool on = srv_config_get_int(CFG_READ_KEEP_BRIGHT, 0) == 0;
    srv_config_set_int(CFG_READ_KEEP_BRIGHT, on ? 1 : 0, true);
    read_refresh();
}

static void on_recent_click(lv_event_t *e)
{
    (void)e;
    bool on = srv_config_get_int(CFG_SHELF_RECENT, CFG_DEF_SHELF_RECENT) == 0;
    srv_config_set_int(CFG_SHELF_RECENT, on ? 1 : 0, true);
    read_refresh();
}

static void on_reset_ok(void *arg)
{
    (void)arg;

    for (int i = 0; i < (int)ITEM_COUNT; i++) {
        srv_config_set_int(s_items[i].key, s_items[i].def, false);
    }
    srv_config_set_int(CFG_READ_KEEP_BRIGHT, 0, false);
    srv_config_set_int(CFG_SHELF_RECENT, CFG_DEF_SHELF_RECENT, false);
    srv_config_save();

    read_refresh();
}

static void on_reset_click(lv_event_t *e)
{
    (void)e;
    ui_dialog_confirm("恢复阅读默认？", "字距/行距/段距都会回到默认值",
                      "恢复", on_reset_ok, NULL);
}

/* ===================== 页面 ===================== */

void ui_set_read_create(lv_obj_t *root)
{
    ui_titlebar_create(root, "阅读", app_navigator_back_cb, NULL);
    lv_obj_t *list = ui_settings_list_create(root);

    for (int i = 0; i < (int)ITEM_COUNT; i++) {
        lv_obj_t *row = ui_row_create(list, UI_ROW_Y(i), s_items[i].icon, s_items[i].name);

        s_val_lb[i] = ui_row_value_create(row, -46, 46);
        ui_row_btn_create(row, -8, 34, "+", on_step, (void *)(intptr_t)(i * 2 + 1));
        ui_row_btn_create(row, -94, 34, "-", on_step, (void *)(intptr_t)(i * 2 + 0));
    }

    lv_obj_t *row = ui_row_create(list, UI_ROW_Y(ITEM_COUNT), &ic_book_open, "阅读时不暗屏");
    s_keep_lb = ui_row_switch_create(row, false, on_keep_click, NULL);

    /* 越后读的书越靠前；排序记录在 /books/.recent */
    row = ui_row_create(list, UI_ROW_Y(ITEM_COUNT + 1), &ic_book_open, "最近阅读优先");
    s_recent_lb = ui_row_switch_create(row, false, on_recent_click, NULL);

    row = ui_row_create(list, UI_ROW_Y(ITEM_COUNT + 2), NULL, "恢复默认");
    ui_row_btn_create(row, -8, 64, "恢复", on_reset_click, NULL);

    read_refresh();
}
