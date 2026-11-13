#include "ui_bookshelf.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "ui_dialog.h"
#include "app_navigator.h"
#include "srv_library.h"
#include "srv_recent.h"
#include "ui_ime.h"
#include "srv_reader.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_shelf";

#define MARGIN     8
#define CARD_H     96
#define CARD_GAP   8

static lv_coord_t s_card_w = 148;
static int        s_cols   = 2;

/* 长按菜单里的动作。菜单项是按书的情况动态拼的（TXT 没有封面/简介、
 * 只有外部书籍能移出），所以光靠回调里的下标认不出点的是哪一项，
 * 得在下标和动作之间再存一张表。 */
enum {
    ACT_COVER = 0,
    ACT_INTRO,
    ACT_REMOVE,
};

static lv_obj_t *s_list;                              /*!< 列表容器 */
static char      s_cur_path[SRV_FS_PATH_MAX];         /*!< 当前长按的书（弹层回调只带一个参数） */
static char      s_cur_name[SRV_BOOK_NAME_MAX];
static int       s_menu_act[UI_MENU_MAX_ITEMS];
static uint32_t  s_menu_tick;                         /*!< 上次长按弹菜单的时刻 */

/* 点卡片：打开这本书，然后进阅读器 */
static void on_book(lv_event_t *e)
{
    /* 长按弹菜单后松手，LVGL 还会补一个 CLICKED（源码里 CLICKED 是无条件发的），
     * 吞掉它 —— 否则菜单刚弹出来书就被打开了。用时刻比对而不是布尔标志：
     * 用户长按后把手指滑出卡片再松开就不会有 CLICKED，标志会一直留着，
     * 把下一次正常点击吃掉。 */
    if (s_menu_tick != 0 && lv_tick_elaps(s_menu_tick) < 800) {
        s_menu_tick = 0;
        return;
    }
    s_menu_tick = 0;

    const srv_book_t *bk = (const srv_book_t *)lv_event_get_user_data(e);
    if (bk == NULL) {
        return;
    }

    /* 打开要数章节、读元数据、把第一章整章读进来，全是 SD 上的慢活 ——
     * 同步做会把 LVGL 任务占住，表现就是「点了半天没反应」。丢给后台任务
     * 之后这里立刻切页，页面上先显示「正在打开」，读完再接上正文。 */
    esp_err_t ret = srv_reader_open_async(bk->path);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "打开失败 %s: %s", bk->path, esp_err_to_name(ret));
        /* 上一次打开还没跑完（用户按了取消就点下一本）时是这个错 */
        ui_alert_show("打开失败", UI_COLOR_WARN,
                      (ret == ESP_ERR_INVALID_STATE) ? "上一本书还在打开，稍等一下"
                                                     : esp_err_to_name(ret),
                      "知道了", NULL, NULL, 2200);
        return;
    }

    app_navigator_switch(PAGE_READER, true);

    /* 记录最近阅读（排序文件在 /books/.recent） */
    srv_recent_touch(bk->path);
}

/* ===================== 长按菜单 ===================== */

static void shelf_rebuild(void);        /* 移出书架后要重排列表，先声明 */

static void act_show_cover(void)
{
    const srv_book_t *bk = NULL;
    if (srv_library_find(s_cur_path, &bk) != ESP_OK || bk == NULL) {
        return;
    }

    uint8_t *data  = NULL;
    size_t   bytes = 0;
    int32_t  w     = 0;
    int32_t  h     = 0;

    esp_err_t ret = srv_library_read_cover(bk, &data, &bytes, &w, &h);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "读取封面失败 %s: %s", s_cur_path, esp_err_to_name(ret));
        ui_alert_show("读取封面失败", UI_COLOR_WARN, s_cur_name, "知道了", NULL, NULL, 2000);
        return;
    }

    /* 封面按物理竖屏铺满整屏显示，缓冲所有权交给弹层 */
    ui_cover_show(data, bytes, w, h);
}

static void act_show_intro(void)
{
    const srv_book_t *bk = NULL;
    if (srv_library_find(s_cur_path, &bk) != ESP_OK || bk == NULL) {
        return;
    }

    /* 简介文本可能上千字节，放 static 不占 LVGL 任务栈（同一时刻只可能
     * 有一个简介层）；容量要跟转换工具的 INTRO_MAX_BYTES 对齐 */
    static char intro[2048];
    esp_err_t ret = srv_library_read_intro(bk, intro, sizeof(intro));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "读取简介失败 %s: %s", s_cur_path, esp_err_to_name(ret));
        ui_alert_show("读取简介失败", UI_COLOR_WARN, s_cur_name, "知道了", NULL, NULL, 2000);
        return;
    }

    ui_info_show(s_cur_name, intro);
}

static void act_remove_ok(void *arg)
{
    (void)arg;

    if (srv_library_remove(s_cur_path) != ESP_OK) {
        ESP_LOGW(TAG, "移出书架失败: %s", s_cur_path);
        ui_alert_show("移出失败", UI_COLOR_WARN, s_cur_name, "知道了", NULL, NULL, 2000);
        return;
    }
    shelf_rebuild();
}

static void act_remove(void)
{
    /* 只动记录不动源文件，但还是二次确认一下：误触的代价是"书不见了" */
    ui_dialog_confirm("移出书架", "只从书架移除，不删除源文件", "移出",
                      act_remove_ok, NULL);
}

static void on_menu_pick(int idx, void *arg)
{
    (void)arg;
    if (idx < 0 || idx >= UI_MENU_MAX_ITEMS) {
        return;                         /* 取消，或点了菜单外的空白 */
    }

    switch (s_menu_act[idx]) {
    case ACT_COVER:
        act_show_cover();
        break;
    case ACT_INTRO:
        act_show_intro();
        break;
    case ACT_REMOVE:
        act_remove();
        break;
    default:
        break;
    }
}

/* 长按卡片：按这本书有什么可做的，拼出菜单 */
static void on_book_long(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const srv_book_t *bk = NULL;
    if (srv_library_get(idx, &bk) != ESP_OK || bk == NULL) {
        return;
    }

    s_menu_tick = lv_tick_get();        /* 让 on_book 吞掉稍后的那次 CLICKED */

    snprintf(s_cur_path, sizeof(s_cur_path), "%s", bk->path);
    snprintf(s_cur_name, sizeof(s_cur_name), "%s", bk->name);

    ui_menu_item_t items[UI_MENU_MAX_ITEMS];
    int n = 0;

    if (bk->type == BOOK_TYPE_EBF && bk->cover_bytes > 0) {
        items[n].text = "查看封面";
        items[n].icon = &ic_cover;
        s_menu_act[n++] = ACT_COVER;
    }
    if (bk->type == BOOK_TYPE_EBF && bk->intro[0] != '\0') {
        items[n].text = "查看简介";
        items[n].icon = &ic_info;
        s_menu_act[n++] = ACT_INTRO;
    }
    if (bk->external) {
        items[n].text = "移出书架";
        items[n].icon = &ic_delete;
        s_menu_act[n++] = ACT_REMOVE;
    }

    if (n == 0) {
        /* 比如 /books 下的 TXT：既没有封面简介，也不允许移出 */
        ui_alert_show("没有可执行的操作", UI_COLOR_TEXT_DIM, s_cur_name,
                      "知道了", NULL, NULL, 1500);
        return;
    }

    ui_menu_show(s_cur_name, items, n, on_menu_pick, NULL);
}

static void add_card(lv_obj_t *list, const srv_book_t *bk, int idx)
{
    int col = idx % s_cols;
    int row = idx / s_cols;

    lv_obj_t *card = lv_obj_create(list);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(card, s_card_w, CARD_H);
    lv_obj_set_pos(card, MARGIN + col * (s_card_w + CARD_GAP),
                   MARGIN + row * (CARD_H + CARD_GAP));
    lv_obj_set_style_bg_color(card, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 8, 0);

    ui_add_click_feedback(card);
    lv_obj_add_event_cb(card, on_book, LV_EVENT_CLICKED, (void *)bk);
    /* 长按弹菜单。传序号而不是 bk 指针：移出书架会让 srv_library 里的数组
     * 整体前移，指针指向的内容就不是这本书了 */
    lv_obj_add_event_cb(card, on_book_long, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)idx);

    const lv_coord_t inner_w = s_card_w - 16;   /* 卡片左右各 8 的内边距 */

    /* 第一行：左边格式图标，右边类型文字。
     * 类型原来是压在卡片底部占一整行的，挪到图标右边之后，下面腾出来的
     * 空间就能给书名排两行了。 */
    lv_obj_t *ic = lv_image_create(card);
    lv_image_set_src(ic, (bk->type == BOOK_TYPE_EBF) ? &ic_file_eb : &ic_file_txt);
    lv_obj_set_style_image_recolor(ic, UI_COLOR_PRIMARY, 0);
    lv_obj_set_style_radius(ic, 0, 0);
    lv_obj_align(ic, LV_ALIGN_TOP_LEFT, 0, 0);

    char meta[32];
    if (bk->type == BOOK_TYPE_EBF) {
        snprintf(meta, sizeof(meta), "EBF · %d章", (int)bk->chapter_count);
    } else if (bk->author[0] != '\0') {
        snprintf(meta, sizeof(meta), "TXT · %.*s", 8, bk->author);
    } else {
        snprintf(meta, sizeof(meta), "TXT");
    }

    lv_obj_t *sub = lv_label_create(card);
    lv_obj_set_style_text_color(sub, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(sub, &font_ui_14, 0);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sub, inner_w - 36);        /* 给左边的图标留位置 */
    lv_label_set_text(sub, meta);
    /* 图标 32 高、这行字 14 高，往下 9px 让两者中线对齐 */
    lv_obj_align(sub, LV_ALIGN_TOP_RIGHT, 0, 9);

    /* 第二行起：书名，最多两行 */
    lv_obj_t *name = lv_label_create(card);
    lv_obj_set_style_text_color(name, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(name, &font_ui_14, 0);
    lv_obj_set_width(name, inner_w);
    /* 卡片内容区只有 80px 高（96 减上下各 8），图标占 32，
     * 剩下 44px 给两行书名 —— 起点放 36 是为了给行高留点余量 */
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 36);

    /* 两行也放不下的长书名改成循环滚动，否则后半截永远看不到。
     * 判断用「单行渲染宽度 > 两行可用宽度」，比按字数估准。
     * 量宽只能走 lv_text_get_size：lv_text_get_width 在 v9 里是 private 的
     * （声明挪进了 lv_text_private.h），公开头里只有 get_size 这一个入口。 */
    lv_point_t sz = { 0, 0 };
    lv_text_get_size(&sz, bk->name, &font_ui_14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (sz.x <= inner_w * 2) {
        lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
    } else {
        lv_obj_set_height(name, 18);
        lv_label_set_long_mode(name, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_anim_duration(name, 6000, 0);  /* 滚一圈 6 秒 */
    }
    lv_label_set_text(name, bk->name);
}

/* ===================== 页面 ===================== */

/* 列表内容（空态或卡片）。抽出来是为了移出书架后能整块重建 */
static char s_filter[SRV_BOOK_NAME_MAX];

/* 书名子串匹配，ASCII 大小写不敏感（中文按字节直接比） */
static bool name_match(const char *name, const char *kw)
{
    if (kw[0] == '\0') {
        return true;
    }
    const size_t ln = strlen(name);
    const size_t lk = strlen(kw);
    for (size_t i = 0; i + lk <= ln; i++) {
        size_t j = 0;
        for (; j < lk; j++) {
            char a = name[i + j];
            char b = kw[j];
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a + 32);
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b + 32);
            }
            if (a != b) {
                break;
            }
        }
        if (j == lk) {
            return true;
        }
    }
    return false;
}

static void shelf_fill(lv_obj_t *list)
{
    const int n = srv_library_count();
    int shown = 0;

    for (int i = 0; i < n; i++) {
        const srv_book_t *bk = NULL;
        if (srv_library_get(i, &bk) != ESP_OK || bk == NULL) {
            continue;
        }
        if (!name_match(bk->name, s_filter)) {
            continue;
        }
        add_card(list, bk, i);
        shown++;
    }

    if (shown == 0) {
        lv_obj_t *empty = lv_label_create(list);
        lv_obj_set_style_text_color(empty, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_font(empty, &font_ui_14, 0);
        if (n <= 0) {
            lv_label_set_text(empty, "书架是空的\n把 txt 或 EBF 文件夹放进 /books/");
        } else {
            lv_label_set_text(empty, "没有找到匹配的书");
        }
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    ESP_LOGI(TAG, "书架列出 %d / %d 本", shown, n);
}

static void shelf_list_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    /* 手快「返回又进来」时新列表已经建好了，旧回调不能把新指针清掉 */
    if (lv_event_get_target(e) != s_list) {
        return;
    }
    s_list = NULL;
}

static void shelf_rebuild(void)
{
    if (s_list == NULL || !lv_obj_is_valid(s_list)) {
        return;
    }
    /* 卡片里带的是书的序号，整块重建最省事 */
    lv_obj_clean(s_list);
    shelf_fill(s_list);
}

static void on_search_done(const char *text, void *arg)
{
    (void)arg;
    /* 输入空串 = 清除过滤 */
    snprintf(s_filter, sizeof(s_filter), "%s", (text != NULL) ? text : "");
    shelf_rebuild();
    ESP_LOGI(TAG, "书架搜索：%s", s_filter);
}

static void on_search_clicked(lv_event_t *e)
{
    (void)e;
    ui_ime_text_input_show("搜索书名", SRV_BOOK_NAME_MAX - 1, on_search_done, NULL);
}

void ui_bookshelf_create(lv_obj_t *root)
{
    const lv_coord_t sw = ui_scr_w();
    const lv_coord_t sh = ui_scr_h();

    /* 横屏双列，竖屏单列。卡片宽度跟着可用宽度算，避免竖屏只显示半张卡。 */
    s_cols = (sw < sh) ? 1 : 2;
    s_card_w = (sw - MARGIN * 2 - (s_cols - 1) * CARD_GAP) / s_cols;

    lv_obj_t *bar = ui_titlebar_create_plain(root, "书架", app_navigator_back_smart_cb, NULL);
    /* 旋转键由标题栏统一放在最右端，搜索键放在它左边 */
    ui_titlebar_button_create(bar, "搜索", -32, 40, on_search_clicked, NULL);

    /* 每次进书架都重扫：最近阅读排序开关可能刚改过 */
    (void)srv_library_scan();

    /* 列表容器：书架比一屏长，要能纵向滚 */
    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_size(list, sw, sh - UI_TITLEBAR_H);
    lv_obj_set_pos(list, 0, UI_TITLEBAR_H);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_bottom(list, MARGIN, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    lv_obj_set_style_width(list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(list, UI_COLOR_TEXT_DIM, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(list, 2, LV_PART_SCROLLBAR);

    s_list = list;
    lv_obj_add_event_cb(list, shelf_list_del_cb, LV_EVENT_DELETE, NULL);

    shelf_fill(list);
}
