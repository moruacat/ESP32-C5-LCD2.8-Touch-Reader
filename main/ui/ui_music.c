#include "ui_music.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "ui_dialog.h"
#include "app_navigator.h"
#include "srv_musiclib.h"
#include "srv_player.h"
#include "srv_audio.h"
#include "srv_sd.h"
#include "srv_fs.h"

#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ui_music";

/* ===================== 布局（横屏 320x240） ===================== */

#define PAGE_LIST_X   8
#define PAGE_LIST_W   304
#define PAGE_LIST_Y   30
#define PAGE_LIST_H   124
#define ROW_H         32      /* 行高按手指点得中来做，别压到 22 那种 */
#define ROW_PAD       8

#define NAME_Y        158
#define BAR_Y         174
#define TIME_Y        184

#define BTN_Y         200
#define BTN_H         36
#define BTN_W         57      /* 5 个键排一排：(304 - 4*4) / 5 */
#define BTN_GAP       4

#define TICK_MS       200

/* 播放列表面板：顶部标签页 + 左边功能栏 + 右边曲目竖列 */
#define PN_TAB_H      32
#define PN_BAR_W      116
#define PN_ROW_H      32
#define PN_ANIM_MS    160

/* 标签页上的序号：走常量表。
 * 用 snprintf("%d") 会被 -Werror=format-truncation 拦（编译器按 int 最大长度算）。 */
static const char *const PL_TAB_TEXT[SRV_MUSIC_LIST_MAX] = {
    "1",  "2",  "3",  "4",  "5",  "6",  "7",  "8",
    "9",  "10", "11", "12", "13", "14", "15", "16",
};

/* ===================== 主页面状态 ===================== */

static lv_obj_t  *s_list      = NULL;
static int         s_queue_len = 0;
static lv_obj_t  **s_rows      = NULL;   /* 列表项（高亮用） */

static lv_obj_t  *s_name_lb = NULL;
static lv_obj_t  *s_pos_lb  = NULL;
static lv_obj_t  *s_dur_lb  = NULL;
static lv_obj_t  *s_slider  = NULL;
static lv_obj_t  *s_play_ic = NULL;

static int  s_last_idx = -2;
static char s_last_name[SRV_MUSIC_NAME_MAX];

/* 只有点「后台播放」退出才让音乐接着放；返回键、右滑都算退出播放 */
static bool s_background_exit = false;

/* ===================== 播放列表面板状态 ===================== */

static lv_obj_t *s_panel      = NULL;
static lv_obj_t *s_pn_tabs    = NULL;
static lv_obj_t *s_pn_col     = NULL;
static lv_obj_t *s_pn_mode    = NULL;   /* 播放方式下拉 */
static bool      s_pn_closing = false;

/* ===================== 加入播放列表的选择层 ===================== */

static lv_obj_t *s_pick = NULL;

/* ===================== 小工具 ===================== */

static void fmt_ms(int ms, char *buf, size_t len)
{
    if (ms < 0) {
        ms = 0;
    }
    int sec = ms / 1000;
    snprintf(buf, len, "%02d:%02d", sec / 60, sec % 60);
}

/* 播放列表没打开过就先打开第一个，否则队列是空的 */
static void ensure_ready(void)
{
    if (!srv_sd_is_mounted()) {
        return;
    }
    if (srv_player_playlist_id() == 0 && srv_musiclib_playlist_count() > 0) {
        (void)srv_player_set_playlist(srv_musiclib_playlist_id(0));
    }
}

/* ===================== 主页面：曲目列表 ===================== */

static void set_row_hl(int idx, bool on)
{
    if (idx < 0 || idx >= s_queue_len || s_rows == NULL || s_rows[idx] == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(s_rows[idx], on ? UI_COLOR_PRIMARY : UI_COLOR_SURFACE, 0);
}

/* 长按操作：记录时刻和曲目路径。LVGL 松手时还会补发一次 CLICKED，
 * 不吞掉就会出现「长按弹菜单的同时开始播放」 */
static uint32_t s_long_tick;
static char     s_long_path[SRV_FS_PATH_MAX];

static bool swallow_click_after_long(void)
{
    if (s_long_tick != 0 && lv_tick_elaps(s_long_tick) < 800) {
        s_long_tick = 0;
        return true;
    }
    s_long_tick = 0;
    return false;
}

static void rebuild_list(void);
static void panel_refresh(void);

/* 主列表长按外部曲目 → 移出媒体库（普通曲目不加长按，避免误触列表顺序） */
static void on_lib_remove_ok(void *arg)
{
    (void)arg;
    if (srv_musiclib_remove(s_long_path) == ESP_OK) {
        ESP_LOGI(TAG, "已移出媒体库: %s", s_long_path);
    }
    rebuild_list();
}

static void on_lib_long(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const srv_music_t *m = NULL;
    if (srv_player_queue_item(idx, &m) != ESP_OK || m == NULL || !m->external) {
        return;
    }
    s_long_tick = lv_tick_get();
    strncpy(s_long_path, m->path, sizeof(s_long_path) - 1);
    s_long_path[sizeof(s_long_path) - 1] = '\0';

    char note[SRV_MUSIC_NAME_MAX + 48];
    snprintf(note, sizeof(note), "「%s」只移出媒体库，不删文件", m->name);
    ui_dialog_confirm("移出媒体库？", note, "移出", on_lib_remove_ok, NULL);
}

static void on_item(lv_event_t *e)
{
    if (swallow_click_after_long()) {
        return;
    }
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    (void)srv_player_play_index(idx);
}

static void rebuild_list(void)
{
    if (s_list == NULL) {
        return;
    }

    lv_obj_clean(s_list);
    free(s_rows);
    s_rows = NULL;

    int n = srv_player_queue_count();
    s_queue_len = n;
    if (n <= 0) {
        return;
    }

    s_rows = calloc((size_t)n, sizeof(lv_obj_t *));
    if (s_rows == NULL) {
        return;
    }

    for (int i = 0; i < n; i++) {
        const srv_music_t *m = NULL;
        if (srv_player_queue_item(i, &m) != ESP_OK || m == NULL) {
            continue;
        }

        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(row, PAGE_LIST_W - 8, ROW_H - 2);
        lv_obj_set_pos(row, 0, (lv_coord_t)(i * ROW_H));
        lv_obj_set_style_bg_color(row, UI_COLOR_SURFACE, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        ui_add_click_feedback(row);
        lv_obj_add_event_cb(row, on_item, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(row, on_lib_long, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);

        lv_obj_t *lb = lv_label_create(row);
        lv_obj_set_style_text_font(lb, &font_ui_14, 0);
        lv_obj_set_style_text_color(lb, UI_COLOR_TEXT, 0);
        lv_obj_set_width(lb, PAGE_LIST_W - 40);
        /* 长曲名循环滚动，别折行（行高只有 30，折行会盖住下一行） */
        lv_label_set_long_mode(lb, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_anim_duration(lb, 6000, 0);
        /* 不要用 lv_label_set_text_fmt：格式没被解析时会原样留着 label 的
         * 默认文本（"Text"），先在本地拼好再 set_text 最稳 */
        char line[SRV_MUSIC_NAME_MAX + 16];
        snprintf(line, sizeof(line), "%d. %s", i + 1, m->name);
        lv_label_set_text(lb, line);
        lv_obj_align(lb, LV_ALIGN_LEFT_MID, ROW_PAD, 0);

        s_rows[i] = row;
    }

    int cur = srv_player_index();
    if (cur > 0 && cur < n) {
        lv_obj_scroll_to_y(s_list, (lv_coord_t)(cur * ROW_H - PAGE_LIST_H / 2), LV_ANIM_OFF);
    }
    set_row_hl(cur, true);
}

/* ===================== 主页面：控制 ===================== */

/* 当前播放键上挂的是不是「暂停」图标：只在真变了才 set_src */
static bool s_play_icon_pause = false;

static void music_set_play_icon(bool want_pause)
{
    if (want_pause == s_play_icon_pause) {
        return;
    }
    s_play_icon_pause = want_pause;
    lv_image_set_src(s_play_ic, want_pause ? &ic_pause : &ic_play);
}

static void on_prev(lv_event_t *e)
{
    (void)e;
    (void)srv_player_prev();
}

static void on_next(lv_event_t *e)
{
    (void)e;
    (void)srv_player_next();
}

static void on_play(lv_event_t *e)
{
    (void)e;

    bool playing = (srv_player_state() == SRV_PLAYER_PLAYING);
    (void)srv_player_toggle();

    /* 命令是异步的，先翻图标给个即时反馈，200ms 后的 tick 会按真实状态校正 */
    music_set_play_icon(!playing);
}

static void on_slider(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_RELEASED && code != LV_EVENT_CLICKED) {
        return;
    }
    (void)srv_player_seek_ms((int)lv_slider_get_value(s_slider));
}

/* 返回按钮上的图标对象（播放键要换图标） */
static lv_obj_t *make_btn(lv_obj_t *parent, lv_coord_t x,
                          const lv_image_dsc_t *icon, lv_event_cb_t cb, bool upside_down)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, BTN_W, BTN_H);
    lv_obj_set_pos(btn, x, BTN_Y);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_bg_color(btn, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    ui_add_click_feedback(btn);
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *ic = lv_image_create(btn);
    lv_image_set_src(ic, icon);
    lv_obj_set_style_radius(ic, 0, 0);
    lv_obj_set_style_image_recolor(ic, UI_COLOR_TEXT, 0);
    if (upside_down) {
        /* 没有「下一曲」图标：拿上一曲的图标转 180 度 */
        lv_image_set_pivot(ic, icon->header.w / 2, icon->header.h / 2);
        lv_image_set_rotation(ic, 1800);
    }
    lv_obj_center(ic);
    return ic;
}

/* ===================== 音量弹层 ===================== */

static lv_obj_t *s_vol     = NULL;
static lv_obj_t *s_vol_val = NULL;

static void vol_close(void)
{
    if (s_vol != NULL) {
        lv_obj_delete(s_vol);
        s_vol = NULL;
        s_vol_val = NULL;
    }
}

static void on_vol_del(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        s_vol = NULL;
        s_vol_val = NULL;
    }
}

static void on_vol_dismiss(lv_event_t *e)
{
    (void)e;
    vol_close();
}

static void on_vol_slider(lv_event_t *e)
{
    int v = (int)lv_slider_get_value(lv_event_get_target(e));
    (void)srv_audio_set_volume(v);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", v);
    if (s_vol_val != NULL) {
        lv_label_set_text(s_vol_val, buf);
    }
}

static void on_vol_open(lv_event_t *e)
{
    (void)e;
    if (s_vol != NULL) {
        return;
    }

    /* 半透明遮罩：点空白处关掉 */
    s_vol = lv_obj_create(lv_screen_active());
    lv_obj_remove_flag(s_vol, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_vol, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(s_vol, 0, 0);
    lv_obj_set_style_bg_color(s_vol, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_vol, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_vol, 0, 0);
    lv_obj_set_style_radius(s_vol, 0, 0);
    lv_obj_set_style_pad_all(s_vol, 0, 0);
    lv_obj_add_event_cb(s_vol, on_vol_dismiss, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_vol, on_vol_del, LV_EVENT_DELETE, NULL);

    lv_obj_t *box = lv_obj_create(s_vol);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);    /* 吃掉点击，别穿透到遮罩 */
    lv_obj_set_size(box, 260, 84);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, UI_COLOR_PRIMARY, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 10, 0);
    lv_obj_set_style_pad_all(box, 0, 0);

    lv_obj_t *ttl = lv_label_create(box);
    lv_obj_set_style_text_font(ttl, &font_ui_14, 0);
    lv_obj_set_style_text_color(ttl, UI_COLOR_TEXT, 0);
    lv_label_set_text(ttl, "音量");
    lv_obj_set_pos(ttl, 14, 8);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", srv_audio_get_volume());
    s_vol_val = lv_label_create(box);
    lv_obj_set_style_text_font(s_vol_val, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_vol_val, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(s_vol_val, buf);
    lv_obj_align(s_vol_val, LV_ALIGN_TOP_RIGHT, -14, 8);

    lv_obj_t *sl = lv_slider_create(box);
    lv_obj_set_size(sl, 220, 10);
    lv_obj_align(sl, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_slider_set_range(sl, 0, 100);
    lv_slider_set_value(sl, srv_audio_get_volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, UI_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, UI_COLOR_PRIMARY, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(sl, on_vol_slider, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ===================== 播放列表面板 ===================== */

static void panel_refresh(void);

static void panel_anim_cb(lv_obj_t *obj, int32_t v)
{
    lv_obj_set_style_translate_x(obj, (lv_coord_t)v, 0);
}

static void panel_closed_cb(lv_anim_t *a)
{
    (void)a;
    if (s_panel != NULL) {
        lv_obj_delete(s_panel);
    }
}

static void panel_close(void)
{
    if (s_panel == NULL || s_pn_closing) {
        return;
    }
    s_pn_closing = true;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_panel);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)panel_anim_cb);
    lv_anim_set_values(&a, 0, UI_SCREEN_W);
    lv_anim_set_duration(&a, PN_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&a, panel_closed_cb);
    lv_anim_start(&a);
}

/* 面板整体右下滑走的手势：横向位移够大、且明显比纵向大才认 */
static lv_point_t s_pn_press;
static bool       s_pn_tracking = false;

static void on_pn_pressed(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (indev != NULL) {
        lv_indev_get_point(indev, &s_pn_press);
        s_pn_tracking = true;
    }
}

static void on_pn_released(lv_event_t *e)
{
    (void)e;
    if (!s_pn_tracking) {
        return;
    }
    s_pn_tracking = false;

    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    int dx = (int)p.x - (int)s_pn_press.x;
    int dy = (int)p.y - (int)s_pn_press.y;
    if (dx > 60 && dx > (dy < 0 ? -dy : dy) * 2) {
        panel_close();
    }
}

static void on_pn_lost(lv_event_t *e)
{
    (void)e;
    s_pn_tracking = false;
}

static void on_tab(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    if (id == srv_player_playlist_id()) {
        return;
    }
    if (srv_player_set_playlist(id) == ESP_OK) {
        panel_refresh();
        rebuild_list();
    }
}

static void on_tab_new(lv_event_t *e)
{
    (void)e;
    int id = 0;
    if (srv_musiclib_playlist_create(&id) == ESP_OK) {
        (void)srv_player_set_playlist(id);
        panel_refresh();
        rebuild_list();
    }
}

static void on_pn_mode(lv_event_t *e)
{
    (void)e;
    int sel = (int)lv_dropdown_get_selected(s_pn_mode);
    if (sel >= 0 && sel < SRV_PLAYER_MODE_NUM) {
        srv_player_set_mode((srv_player_mode_t)sel);
        rebuild_list();                 /* 队列来源可能变了 */
    }
}

/* 删除当前播放列表 */
static void on_del_ok(void *arg)
{
    (void)arg;
    int id = srv_player_playlist_id();
    if (id == 0) {
        return;
    }

    if (srv_musiclib_playlist_delete(id) != ESP_OK) {
        return;
    }

    /* 删完切到剩下的第一个；一个都不剩就建个空的，界面别空着 */
    int cnt = srv_musiclib_playlist_count();
    if (cnt > 0) {
        (void)srv_player_set_playlist(srv_musiclib_playlist_id(0));
    } else {
        int nid = 0;
        if (srv_musiclib_playlist_create(&nid) == ESP_OK) {
            (void)srv_player_set_playlist(nid);
        }
    }

    panel_refresh();
    rebuild_list();
}

static void on_pn_del(lv_event_t *e)
{
    (void)e;
    ui_dialog_confirm("删除这个播放列表？", "只删列表，不会删除音乐文件",
                      "删除", on_del_ok, NULL);
}

static void on_pn_bg(lv_event_t *e)
{
    (void)e;
    s_background_exit = true;       /* 这次退出要接着放 */
    panel_close();
    app_navigator_back();
}

static void on_pn_close(lv_event_t *e)
{
    (void)e;
    panel_close();
}

static int s_long_idx;

/* 普通曲目：只移出播放列表（不删文件） */
static void on_remove_ok(void *arg)
{
    int idx = (int)(intptr_t)arg;
    if (srv_musiclib_playlist_remove_item(idx) == ESP_OK) {
        ESP_LOGI(TAG, "移出播放列表第 %d 首", idx + 1);
    }
    panel_refresh();
    rebuild_list();
}

/* 播放列表面板的长按菜单：0=移出播放列表，1=移出媒体库 */
static void on_pn_long_menu(int choice, void *arg)
{
    (void)arg;
    if (choice == 0) {
        if (srv_musiclib_playlist_remove_item(s_long_idx) == ESP_OK) {
            ESP_LOGI(TAG, "移出播放列表第 %d 首", s_long_idx + 1);
        }
    } else if (choice == 1) {
        if (srv_musiclib_remove(s_long_path) == ESP_OK) {
            ESP_LOGI(TAG, "已移出媒体库: %s", s_long_path);
        }
    } else {
        return;                         /* 取消 */
    }
    panel_refresh();
    rebuild_list();
}

/* 长按曲目：普通曲目只移出列表；外部曲目额外给「移出媒体库」 */
static void on_pn_item_long(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);

    const srv_music_t *m = NULL;
    if (srv_musiclib_playlist_item(idx, &m) != ESP_OK || m == NULL) {
        return;
    }

    s_long_tick = lv_tick_get();
    s_long_idx  = idx;
    strncpy(s_long_path, m->path, sizeof(s_long_path) - 1);
    s_long_path[sizeof(s_long_path) - 1] = '\0';

    if (m->external) {
        static const ui_menu_item_t items[] = {
            {"移出播放列表", NULL},
            {"移出媒体库",   NULL},
        };
        ui_menu_show(m->name, items, 2, on_pn_long_menu, NULL);
        return;
    }

    char note[SRV_MUSIC_NAME_MAX + 48];
    snprintf(note, sizeof(note), "「%s」只是移出，不删文件", m->name);
    ui_dialog_confirm("移出播放列表？", note, "移出", on_remove_ok, (void *)(intptr_t)idx);
}

static void on_pn_item(lv_event_t *e)
{
    if (swallow_click_after_long()) {
        return;
    }
    int idx = (int)(intptr_t)lv_event_get_user_data(e);

    const srv_music_t *m = NULL;
    if (srv_musiclib_playlist_item(idx, &m) != ESP_OK || m == NULL) {
        return;
    }

    /* 在当前队列里按路径找到它再播：队列可能是播放列表，也可能是媒体库全部 */
    int n = srv_player_queue_count();
    for (int i = 0; i < n; i++) {
        const srv_music_t *q = NULL;
        if (srv_player_queue_item(i, &q) == ESP_OK && q != NULL &&
            strcmp(q->path, m->path) == 0) {
            (void)srv_player_play_index(i);
            break;
        }
    }
    panel_close();
}

/* 标签页 + 曲目竖列一起重建 */
static void panel_refresh(void)
{
    if (s_panel == NULL) {
        return;
    }

    if (s_pn_mode != NULL) {
        lv_dropdown_set_selected(s_pn_mode, (uint16_t)srv_player_mode());
    }

    /* ---- 标签页 ---- */
    lv_obj_clean(s_pn_tabs);

    int cnt = srv_musiclib_playlist_count();
    int cur = srv_player_playlist_id();

    for (int i = 0; i < cnt && i < SRV_MUSIC_LIST_MAX; i++) {
        int id = srv_musiclib_playlist_id(i);

        lv_obj_t *tab = lv_button_create(s_pn_tabs);
        lv_obj_set_size(tab, 44, 26);
        lv_obj_set_pos(tab, (lv_coord_t)(4 + i * 46), 3);
        lv_obj_set_style_radius(tab, 6, 0);
        lv_obj_set_style_bg_color(tab, (id == cur) ? UI_COLOR_PRIMARY : UI_COLOR_SURFACE, 0);
        lv_obj_set_style_shadow_width(tab, 0, 0);
        lv_obj_set_style_pad_all(tab, 0, 0);
        ui_add_click_feedback(tab);
        lv_obj_add_event_cb(tab, on_tab, LV_EVENT_CLICKED, (void *)(intptr_t)id);

        lv_obj_t *tl = lv_label_create(tab);
        lv_obj_set_style_text_font(tl, &font_ui_14, 0);
        lv_obj_set_style_text_color(tl, UI_COLOR_TEXT, 0);
        lv_label_set_text(tl, PL_TAB_TEXT[i]);
        lv_obj_center(tl);
    }

    /* 新建 */
    lv_obj_t *add = lv_button_create(s_pn_tabs);
    lv_obj_set_size(add, 44, 26);
    lv_obj_set_pos(add, (lv_coord_t)(4 + cnt * 46), 3);
    lv_obj_set_style_radius(add, 6, 0);
    lv_obj_set_style_bg_color(add, UI_COLOR_BG, 0);
    lv_obj_set_style_shadow_width(add, 0, 0);
    lv_obj_set_style_pad_all(add, 0, 0);
    ui_add_click_feedback(add);
    lv_obj_add_event_cb(add, on_tab_new, LV_EVENT_CLICKED, NULL);
    lv_obj_t *al = lv_label_create(add);
    lv_obj_set_style_text_font(al, &font_ui_14, 0);
    lv_obj_set_style_text_color(al, UI_COLOR_TEXT, 0);
    lv_label_set_text(al, "+");
    lv_obj_center(al);

    /* ---- 曲目竖列（当前播放列表的内容） ---- */
    lv_obj_clean(s_pn_col);

    int n = srv_musiclib_playlist_item_count();
    if (n <= 0) {
        lv_obj_t *empty = lv_label_create(s_pn_col);
        lv_obj_set_style_text_font(empty, &font_ui_14, 0);
        lv_obj_set_style_text_color(empty, UI_COLOR_TEXT_DIM, 0);
        lv_label_set_text(empty, "列表是空的\n点「添加曲目」从媒体库选");
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 20);
        return;
    }

    const srv_music_t *playing = srv_player_current();
    const lv_coord_t   col_w   = UI_SCREEN_W - PN_BAR_W;

    for (int i = 0; i < n; i++) {
        const srv_music_t *m = NULL;
        if (srv_musiclib_playlist_item(i, &m) != ESP_OK || m == NULL) {
            continue;
        }

        bool is_playing = (playing != NULL && strcmp(playing->path, m->path) == 0);

        lv_obj_t *row = lv_obj_create(s_pn_col);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(row, col_w - 8, PN_ROW_H - 2);
        lv_obj_set_pos(row, 0, (lv_coord_t)(i * PN_ROW_H));
        lv_obj_set_style_bg_color(row, is_playing ? UI_COLOR_PRIMARY : UI_COLOR_SURFACE, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        ui_add_click_feedback(row);
        lv_obj_add_event_cb(row, on_pn_item, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        /* 长按曲目 = 移出这个列表（需求里"长按曲目弹出操作菜单"的那条） */
        lv_obj_add_event_cb(row, on_pn_item_long, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);

        lv_obj_t *lb = lv_label_create(row);
        lv_obj_set_style_text_font(lb, &font_ui_14, 0);
        lv_obj_set_style_text_color(lb, UI_COLOR_TEXT, 0);
        lv_obj_set_width(lb, col_w - 30);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_anim_duration(lb, 6000, 0);
        char line[SRV_MUSIC_NAME_MAX + 16];
        snprintf(line, sizeof(line), "%d. %s", i + 1, m->name);
        lv_label_set_text(lb, line);
        lv_obj_align(lb, LV_ALIGN_LEFT_MID, ROW_PAD, 0);
    }
}

/* ===================== 选曲层（媒体库 -> 播放列表） ===================== */

static void pick_close(void)
{
    if (s_pick != NULL) {
        lv_obj_delete(s_pick);
        s_pick = NULL;
    }
}

static void on_pick_del(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        s_pick = NULL;
    }
}

static void on_pick_item(lv_event_t *e)
{
    const srv_music_t *m = (const srv_music_t *)lv_event_get_user_data(e);
    if (m != NULL) {
        (void)srv_musiclib_playlist_append(m->path);
        ESP_LOGI(TAG, "加入播放列表: %s", m->name);
    }
    pick_close();
    panel_refresh();
    rebuild_list();
}

static void on_pick_close(lv_event_t *e)
{
    (void)e;
    pick_close();
}

static void pick_open(void)
{
    if (s_pick != NULL) {
        return;
    }

    s_pick = lv_obj_create(lv_screen_active());
    lv_obj_remove_flag(s_pick, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_pick, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(s_pick, 0, 0);
    lv_obj_set_style_bg_color(s_pick, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_pick, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pick, 0, 0);
    lv_obj_set_style_radius(s_pick, 0, 0);
    lv_obj_set_style_pad_all(s_pick, 0, 0);
    lv_obj_add_event_cb(s_pick, on_pick_del, LV_EVENT_DELETE, NULL);

    ui_titlebar_create(s_pick, "加入播放列表", on_pick_close, NULL);

    lv_obj_t *tip = lv_label_create(s_pick);
    lv_obj_set_style_text_font(tip, &font_ui_14, 0);
    lv_obj_set_style_text_color(tip, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(tip, "点曲目即加入，可重复添加");
    lv_obj_set_pos(tip, PAGE_LIST_X + 2, UI_TITLEBAR_H + 2);

    lv_obj_t *list = lv_obj_create(s_pick);
    lv_obj_set_size(list, PAGE_LIST_W, UI_SCREEN_H - UI_TITLEBAR_H - 26);
    lv_obj_set_pos(list, PAGE_LIST_X, UI_TITLEBAR_H + 20);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);

    int n = srv_musiclib_count();
    if (n <= 0) {
        lv_obj_t *empty = lv_label_create(list);
        lv_obj_set_style_text_font(empty, &font_ui_14, 0);
        lv_obj_set_style_text_color(empty, UI_COLOR_TEXT_DIM, 0);
        lv_label_set_text(empty, srv_sd_is_mounted() ? "媒体库是空的（/music 下没有音频）"
                                                     : "SD 卡未挂载");
        return;
    }

    for (int i = 0; i < n; i++) {
        const srv_music_t *m = NULL;
        if (srv_musiclib_get(i, &m) != ESP_OK || m == NULL) {
            continue;
        }

        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(row, PAGE_LIST_W - 8, ROW_H - 4);
        lv_obj_set_pos(row, 0, (lv_coord_t)(i * (ROW_H - 2)));
        lv_obj_set_style_bg_color(row, UI_COLOR_SURFACE, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        ui_add_click_feedback(row);
        lv_obj_add_event_cb(row, on_pick_item, LV_EVENT_CLICKED, (void *)m);

        lv_obj_t *lb = lv_label_create(row);
        lv_obj_set_style_text_font(lb, &font_ui_14, 0);
        lv_obj_set_style_text_color(lb, UI_COLOR_TEXT, 0);
        lv_obj_set_width(lb, PAGE_LIST_W - 60);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_anim_duration(lb, 6000, 0);
        lv_label_set_text(lb, m->name);
        lv_obj_align(lb, LV_ALIGN_LEFT_MID, ROW_PAD, 0);

        lv_obj_t *ext = lv_label_create(row);
        lv_obj_set_style_text_font(ext, &font_ui_14, 0);
        lv_obj_set_style_text_color(ext, UI_COLOR_TEXT_DIM, 0);
        lv_label_set_text(ext, m->external ? "外" : "");
        lv_obj_align(ext, LV_ALIGN_RIGHT_MID, -ROW_PAD, 0);
    }
}

static void on_pn_add(lv_event_t *e)
{
    (void)e;
    pick_open();
}

/* ===================== 面板创建 ===================== */

static void on_panel_del(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    s_panel      = NULL;
    s_pn_tabs    = NULL;
    s_pn_col     = NULL;
    s_pn_mode    = NULL;
    s_pn_closing = false;
    s_pn_tracking = false;
}

/* 功能栏上的一个键 */
static lv_obj_t *pn_btn(lv_obj_t *parent, lv_coord_t y, const char *text,
                        bool primary, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, PN_BAR_W - 16, 32);
    lv_obj_set_pos(btn, 8, y);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, primary ? UI_COLOR_PRIMARY : UI_COLOR_BG, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    ui_add_click_feedback(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lb = lv_label_create(btn);
    lv_obj_set_style_text_font(lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(lb, primary ? lv_color_white() : UI_COLOR_TEXT, 0);
    lv_label_set_text(lb, text);
    lv_obj_center(lb);
    return btn;
}

static void panel_open(void)
{
    if (s_panel != NULL || !srv_sd_is_mounted()) {
        return;
    }

    s_pn_closing = false;

    s_panel = lv_obj_create(lv_screen_active());
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_panel, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(s_panel, 0, 0);
    lv_obj_set_style_bg_color(s_panel, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_set_style_pad_all(s_panel, 0, 0);
    lv_obj_add_event_cb(s_panel, on_panel_del, LV_EVENT_DELETE, NULL);
    /* 右滑把面板推走；起手落在列表/下拉上也交给它自己处理 */
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_panel, on_pn_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_panel, on_pn_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_panel, on_pn_lost, LV_EVENT_PRESS_LOST, NULL);

    /* ---- 顶部标签页 ---- */
    s_pn_tabs = lv_obj_create(s_panel);
    lv_obj_set_size(s_pn_tabs, UI_SCREEN_W, PN_TAB_H);
    lv_obj_set_pos(s_pn_tabs, 0, 0);
    lv_obj_set_style_bg_color(s_pn_tabs, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_pn_tabs, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pn_tabs, 0, 0);
    lv_obj_set_style_radius(s_pn_tabs, 0, 0);
    lv_obj_set_style_pad_all(s_pn_tabs, 0, 0);
    lv_obj_set_scroll_dir(s_pn_tabs, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_pn_tabs, LV_SCROLLBAR_MODE_OFF);
    /* 让标签页里按钮的按下/抬起冒泡到面板，右滑关闭手势才能生效 */
    lv_obj_add_flag(s_pn_tabs, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* ---- 左边功能栏 ---- */
    lv_obj_t *bar = lv_obj_create(s_panel);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bar, PN_BAR_W, UI_SCREEN_H - PN_TAB_H);
    lv_obj_set_pos(bar, 0, PN_TAB_H);
    lv_obj_set_style_bg_color(bar, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *mlb = lv_label_create(bar);
    lv_obj_set_style_text_font(mlb, &font_ui_14, 0);
    lv_obj_set_style_text_color(mlb, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(mlb, "播放方式");
    lv_obj_set_pos(mlb, 8, 0);

    s_pn_mode = lv_dropdown_create(bar);
    lv_obj_set_size(s_pn_mode, PN_BAR_W - 16, 32);
    lv_obj_set_pos(s_pn_mode, 8, 16);
    lv_dropdown_set_options(s_pn_mode,
                            "全部循环\n列表循环\n单曲循环\n随机播放");
    /* 默认箭头是 LV_SYMBOL_DOWN（montserrat 自带符号），全局字体换成
     * font_ui_14 后没有那个字形，显示成空白 —— 用中文字体里有的 ASCII 字符 */
    lv_dropdown_set_symbol(s_pn_mode, "v");
    lv_dropdown_set_selected(s_pn_mode, (uint16_t)srv_player_mode());
    lv_obj_set_style_bg_color(s_pn_mode, UI_COLOR_BG, 0);
    lv_obj_set_style_text_color(s_pn_mode, UI_COLOR_TEXT, 0);
    lv_obj_set_style_border_width(s_pn_mode, 0, 0);
    lv_obj_set_style_radius(s_pn_mode, 6, 0);
    lv_obj_set_style_pad_left(s_pn_mode, 6, 0);
    lv_obj_add_event_cb(s_pn_mode, on_pn_mode, LV_EVENT_VALUE_CHANGED, NULL);

    (void)pn_btn(bar, 50,  "添加曲目", true,  on_pn_add);
    (void)pn_btn(bar, 84,  "删除列表", false, on_pn_del);
    (void)pn_btn(bar, 118, "后台播放", false, on_pn_bg);
    (void)pn_btn(bar, 152, "关闭",     false, on_pn_close);

    lv_obj_t *tip = lv_label_create(bar);
    lv_obj_set_style_text_font(tip, &font_ui_14, 0);
    lv_obj_set_style_text_color(tip, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_width(tip, PN_BAR_W - 16);
    lv_label_set_long_mode(tip, LV_LABEL_LONG_WRAP);
    lv_label_set_text(tip, "长按曲目可移出");
    lv_obj_set_pos(tip, 8, 188);

    /* ---- 右边曲目竖列 ---- */
    s_pn_col = lv_obj_create(s_panel);
    lv_obj_set_size(s_pn_col, UI_SCREEN_W - PN_BAR_W, UI_SCREEN_H - PN_TAB_H);
    lv_obj_set_pos(s_pn_col, PN_BAR_W, PN_TAB_H);
    lv_obj_set_style_bg_opa(s_pn_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pn_col, 0, 0);
    lv_obj_set_style_pad_all(s_pn_col, 4, 0);
    lv_obj_set_scrollbar_mode(s_pn_col, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_scroll_dir(s_pn_col, LV_DIR_VER);
    lv_obj_add_flag(s_pn_col, LV_OBJ_FLAG_EVENT_BUBBLE);

    panel_refresh();

    /* ---- 从右往左滑入 ---- */
    lv_obj_set_style_translate_x(s_panel, UI_SCREEN_W, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_panel);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)panel_anim_cb);
    lv_anim_set_values(&a, UI_SCREEN_W, 0);
    lv_anim_set_duration(&a, PN_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void on_open_panel(lv_event_t *e)
{
    (void)e;
    panel_open();
}

/* ===================== 定时刷新 ===================== */

static void music_tick(lv_timer_t *timer)
{
    (void)timer;

    int n = srv_player_queue_count();
    if (n != s_queue_len) {
        rebuild_list();
    }

    int idx = srv_player_index();
    if (idx != s_last_idx) {
        set_row_hl(s_last_idx, false);
        set_row_hl(idx, true);
        s_last_idx = idx;
    }

    const srv_music_t *m = srv_player_current();
    const char *name = (m != NULL) ? m->name : "";
    if (strcmp(s_last_name, name) != 0) {
        snprintf(s_last_name, sizeof(s_last_name), "%s", name);
        lv_label_set_text(s_name_lb, name[0] ? name : "未选择曲目");
        if (idx >= 0 && s_rows != NULL) {
            lv_obj_scroll_to_y(s_list, (lv_coord_t)(idx * ROW_H - PAGE_LIST_H / 2), LV_ANIM_OFF);
        }
    }

    int dur = srv_player_duration_ms();
    int pos = srv_player_position_ms();

    char buf[16];
    fmt_ms(pos, buf, sizeof(buf));
    lv_label_set_text(s_pos_lb, buf);
    fmt_ms(dur, buf, sizeof(buf));
    lv_label_set_text(s_dur_lb, dur > 0 ? buf : "--:--");

    if (!lv_slider_is_dragged(s_slider)) {
        /* 时长还没算出来时（例如刚恢复到上次进度、还没解码出码率），
         * 就先拿已恢复的位置当量程，否则进度条会把当前位置夹到 100% */
        int rng = (dur > 0) ? dur : (pos > 0 ? pos : 1);
        lv_slider_set_range(s_slider, 0, rng);
        lv_slider_set_value(s_slider, pos, LV_ANIM_OFF);
    }

    music_set_play_icon(srv_player_state() == SRV_PLAYER_PLAYING);
}

static void on_root_del(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    lv_timer_t *t = (lv_timer_t *)lv_event_get_user_data(e);
    if (t != NULL) {
        lv_timer_delete(t);
    }

    /* 不是「后台播放」退出的，就把音乐停掉并释放音频通道 */
    if (!s_background_exit && srv_player_state() != SRV_PLAYER_IDLE) {
        (void)srv_player_stop();
    }
    s_background_exit = false;

    free(s_rows);
    s_rows = NULL;
    s_list = NULL;
    s_name_lb = NULL;
    s_pos_lb = NULL;
    s_dur_lb = NULL;
    s_slider = NULL;
    s_play_ic = NULL;
    s_panel = NULL;
    s_pn_tabs = NULL;
    s_pn_col = NULL;
    s_pn_mode = NULL;
    s_pick = NULL;
    s_vol = NULL;
    s_vol_val = NULL;
    s_queue_len = 0;
    s_last_idx = -2;
    s_last_name[0] = '\0';
    s_play_icon_pause = false;
}

/* ===================== 页面 ===================== */

void ui_music_create(lv_obj_t *root)
{
    s_background_exit = false;      /* 上一轮的退出标记不能带进来 */
    s_play_icon_pause = false;      /* 播放键初始挂的是「播放」图标 */
    s_last_idx = -2;
    s_last_name[0] = '\0';

    ui_titlebar_create(root, "音乐", app_navigator_back_smart_cb, NULL);

    /* 时段静音：只在「本来没在放音乐」时判定。后台已经在放（进页只是看看列表/
     * 换歌），一进门把音量压到 0 会突然没声，不符合手机的使用习惯。 */
    if (srv_player_state() == SRV_PLAYER_IDLE) {
        (void)srv_audio_apply_time_mute();
    }

    ensure_ready();

    /* 曲目列表 */
    s_list = lv_obj_create(root);
    lv_obj_set_size(s_list, PAGE_LIST_W, PAGE_LIST_H);
    lv_obj_set_pos(s_list, PAGE_LIST_X, PAGE_LIST_Y);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    /* 当前曲名：长名字循环滚动 */
    s_name_lb = lv_label_create(root);
    lv_obj_set_style_text_font(s_name_lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_name_lb, UI_COLOR_TEXT, 0);
    lv_obj_set_width(s_name_lb, PAGE_LIST_W);
    lv_obj_set_pos(s_name_lb, PAGE_LIST_X, NAME_Y);
    lv_label_set_long_mode(s_name_lb, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(s_name_lb, 6000, 0);
    lv_label_set_text(s_name_lb, "未选择曲目");

    /* 进度条 */
    s_slider = lv_slider_create(root);
    lv_obj_set_size(s_slider, PAGE_LIST_W, 8);
    lv_obj_set_pos(s_slider, PAGE_LIST_X, BAR_Y);
    lv_slider_set_range(s_slider, 0, 1);
    lv_slider_set_value(s_slider, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_slider, UI_COLOR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider, UI_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_slider, UI_COLOR_PRIMARY, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_slider, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(s_slider, on_slider, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_slider, on_slider, LV_EVENT_CLICKED, NULL);

    /* 时间 */
    s_pos_lb = lv_label_create(root);
    lv_obj_set_style_text_font(s_pos_lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_pos_lb, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(s_pos_lb, "00:00");
    lv_obj_set_pos(s_pos_lb, PAGE_LIST_X, TIME_Y);

    s_dur_lb = lv_label_create(root);
    lv_obj_set_style_text_font(s_dur_lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_dur_lb, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(s_dur_lb, "--:--");
    lv_obj_align(s_dur_lb, LV_ALIGN_TOP_RIGHT, -PAGE_LIST_X, TIME_Y);

    /* 控制键：上一曲 / 播放暂停 / 下一曲 / 音量 / 播放列表
     * （停止只在时钟页那条后台播放控制条上） */
    const lv_coord_t step = BTN_W + BTN_GAP;
    (void)make_btn(root, PAGE_LIST_X, &ic_prev, on_prev, false);

    /* 播放/暂停键：主色实心，图标要随状态换，所以单独建 */
    lv_obj_t *play_btn = lv_button_create(root);
    lv_obj_set_size(play_btn, BTN_W, BTN_H);
    lv_obj_set_pos(play_btn, PAGE_LIST_X + step, BTN_Y);
    lv_obj_set_style_radius(play_btn, 8, 0);
    lv_obj_set_style_bg_color(play_btn, UI_COLOR_PRIMARY, 0);
    lv_obj_set_style_shadow_width(play_btn, 0, 0);
    lv_obj_set_style_pad_all(play_btn, 0, 0);
    ui_add_click_feedback(play_btn);
    lv_obj_add_event_cb(play_btn, on_play, LV_EVENT_CLICKED, NULL);

    s_play_ic = lv_image_create(play_btn);
    lv_image_set_src(s_play_ic, &ic_play);
    lv_obj_set_style_image_recolor(s_play_ic, lv_color_white(), 0);
    lv_obj_set_style_radius(s_play_ic, 0, 0);
    lv_obj_center(s_play_ic);

    (void)make_btn(root, PAGE_LIST_X + step * 2, &ic_prev, on_next, true);
    (void)make_btn(root, PAGE_LIST_X + step * 3, &ic_volume, on_vol_open, false);
    (void)make_btn(root, PAGE_LIST_X + step * 4, &ic_list, on_open_panel, false);

    rebuild_list();
    music_tick(NULL);

    lv_timer_t *tmr = lv_timer_create(music_tick, TICK_MS, NULL);
    lv_obj_add_event_cb(root, on_root_del, LV_EVENT_DELETE, tmr);
}
