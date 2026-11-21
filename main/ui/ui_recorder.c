#include "ui_recorder.h"
#include "ui_common.h"
#include "app_navigator.h"
#include "srv_recorder.h"
#include "srv_player.h"
#include "srv_sd.h"

#include <stdio.h>
#include <string.h>

#define TICK_MS     200
#define ROW_H       26
#define BTN_H       44
#define BTN_W       200

static lv_obj_t *s_list;        /*!< 录音文件列表 */
static lv_obj_t *s_time_lb;     /*!< 录音时长 / 播放进度 */
static lv_obj_t *s_state_lb;    /*!< 状态一行 */
static lv_obj_t *s_btn;
static lv_obj_t *s_btn_lb;

/*! 正在播放/最近点击的录音下标（用于列表高亮）；-1 = 没有 */
static int s_sel = -1;
/*! 列表里当前画出来的高亮项：和 s_sel 不一致时在定时器里重建（不能在手势回调里删控件） */
static int s_shown_sel = -2;

/* ===================== 小工具 ===================== */

static void fmt_time(char *buf, size_t cap, int64_t ms)
{
    const int64_t sec = (ms > 0) ? ms / 1000 : 0;
    if (sec >= 3600) {
        snprintf(buf, cap, "%d:%02d:%02d",
                 (int)(sec / 3600), (int)((sec / 60) % 60), (int)(sec % 60));
    } else {
        snprintf(buf, cap, "%02d:%02d", (int)(sec / 60), (int)(sec % 60));
    }
}

/* 当前播放的是不是本页的录音（录音机只播 /record 下的文件） */
static bool playing_record(void)
{
    const srv_music_t *m = srv_player_current();
    if (m == NULL || srv_player_state() == SRV_PLAYER_IDLE) {
        return false;
    }
    return strstr(m->path, "/record/") != NULL;
}

/* ===================== 列表 ===================== */

static void file_clicked(int idx)
{
    char path[80];
    char name[24];
    if (srv_recorder_file_info(idx, path, sizeof(path), name, sizeof(name), NULL) != ESP_OK) {
        return;
    }

    if (srv_recorder_is_active()) {
        (void)srv_recorder_stop();          /* 录音时不能同时放，先停 */
    }

    const srv_music_t *cur = srv_player_current();
    const bool same = (cur != NULL && strcmp(cur->path, path) == 0 &&
                       srv_player_state() != SRV_PLAYER_IDLE);

    if (same) {
        (void)srv_player_stop();
        s_sel = -1;
        lv_label_set_text(s_state_lb, "已停止");
        return;
    }

    esp_err_t ret = srv_player_play_file(path);
    if (ret == ESP_OK) {
        s_sel = idx;
        char buf[64];
        snprintf(buf, sizeof(buf), "正在播放 %s", name);
        lv_label_set_text(s_state_lb, buf);
    } else {
        const char *why = "播放失败";
        if (ret == ESP_ERR_NOT_FOUND) {
            why = "文件不存在";
        } else if (ret == ESP_ERR_NOT_SUPPORTED) {
            why = "不支持的格式";
        } else if (ret == ESP_ERR_INVALID_STATE) {
            why = "SD 卡未挂载";
        }
        lv_label_set_text(s_state_lb, why);
    }
}

static void on_row_clicked(lv_event_t *e)
{
    file_clicked((int)(intptr_t)lv_event_get_user_data(e));
}

static void list_rebuild(void)
{
    if (s_list == NULL || !lv_obj_is_valid(s_list)) {
        return;
    }
    lv_obj_clean(s_list);

    const int n = srv_recorder_scan_files();
    const lv_coord_t w = lv_obj_get_width(s_list) - 4;
    s_shown_sel = s_sel;

    if (n <= 0) {
        lv_obj_t *lb = lv_label_create(s_list);
        lv_obj_set_style_text_color(lb, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_font(lb, &font_ui_14, 0);
        lv_label_set_text(lb, srv_sd_is_mounted()
                              ? "还没有录音"
                              : "SD 卡未挂载");
        lv_obj_align(lb, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    for (int i = 0; i < n; i++) {
        char name[24];
        uint32_t bytes = 0;
        if (srv_recorder_file_info(i, NULL, 0, name, sizeof(name), &bytes) != ESP_OK) {
            continue;
        }

        lv_obj_t *row = lv_button_create(s_list);
        lv_obj_set_size(row, w, ROW_H - 2);
        lv_obj_set_pos(row, 2, i * ROW_H + 1);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_bg_color(row,
                                  (i == s_sel) ? UI_COLOR_PRIMARY : UI_COLOR_SURFACE, 0);
        ui_add_click_feedback(row);
        lv_obj_add_event_cb(row, on_row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *nm = lv_label_create(row);
        lv_obj_set_style_text_font(nm, &font_ui_14, 0);
        lv_obj_set_style_text_color(nm, UI_COLOR_TEXT, 0);
        lv_label_set_text(nm, name);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 10, 0);

        char sz[16];
        if (bytes >= 1048576) {
            snprintf(sz, sizeof(sz), "%uM", (unsigned)(bytes / 1048576));
        } else {
            snprintf(sz, sizeof(sz), "%uK", (unsigned)(bytes / 1024));
        }
        lv_obj_t *kb = lv_label_create(row);
        lv_obj_set_style_text_font(kb, &font_ui_14, 0);
        lv_obj_set_style_text_color(kb, UI_COLOR_TEXT_DIM, 0);
        lv_label_set_text(kb, sz);
        lv_obj_align(kb, LV_ALIGN_RIGHT_MID, -10, 0);
    }
}

/* ===================== 刷新 ===================== */

static void rec_update(void)
{
    const bool recording = srv_recorder_is_active();

    char buf[16];
    if (playing_record()) {
        fmt_time(buf, sizeof(buf), srv_player_position_ms());
        lv_label_set_text(s_time_lb, buf);

        const srv_music_t *m = srv_player_current();
        char note[64];
        snprintf(note, sizeof(note), "正在播放 %s（再点一下停止）",
                 (m != NULL) ? m->name : "录音");
        lv_label_set_text(s_state_lb, note);
    } else if (recording) {
        fmt_time(buf, sizeof(buf), srv_recorder_elapsed_ms());
        lv_label_set_text(s_time_lb, buf);
        lv_label_set_text(s_state_lb, "录音中…");
    } else {
        fmt_time(buf, sizeof(buf), srv_recorder_elapsed_ms());
        lv_label_set_text(s_time_lb, buf);
    }

    if (recording) {
        lv_label_set_text(s_btn_lb, "停止录音");
        lv_obj_set_style_bg_color(s_btn, UI_COLOR_TEMP, 0);
    } else {
        lv_label_set_text(s_btn_lb, "开始录音");
        lv_obj_set_style_bg_color(s_btn, UI_COLOR_PRIMARY, 0);
    }

    /* 播放自然结束（单曲播完）后把高亮清掉 */
    if (s_sel >= 0 && !playing_record()) {
        s_sel = -1;
    }

    /* 高亮变化不能在点击回调里重建列表（会把正在派发事件的行删掉），
     * 这里在 200ms 定时器里做 */
    if (s_sel != s_shown_sel) {
        list_rebuild();
    }
}

static void rec_tick_cb(lv_timer_t *timer)
{
    (void)timer;
    rec_update();
}

/* ===================== 交互 ===================== */

static void on_btn(lv_event_t *e)
{
    (void)e;

    if (srv_recorder_is_active()) {
        srv_recorder_stop();
        rec_update();

        /* 停完显示存到哪儿了（只取文件名，完整路径太长） */
        const char *path = srv_recorder_last_file();
        const char *name = strrchr(path, '/');
        char buf[80];
        snprintf(buf, sizeof(buf), "已保存 /record/%s", name ? name + 1 : path);
        lv_label_set_text(s_state_lb, buf);

        list_rebuild();
        return;
    }

    /* 录音前先停掉试听，避免麦克风和扬声器同时开 */
    if (srv_player_state() != SRV_PLAYER_IDLE) {
        (void)srv_player_stop();
        s_sel = -1;
    }

    esp_err_t ret = srv_recorder_start();
    if (ret != ESP_OK) {
        lv_label_set_text(s_state_lb,
                          srv_sd_is_mounted() ? "启动失败" : "SD 卡未挂载");
        return;
    }
    list_rebuild();
    rec_update();
}

static void rec_root_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    lv_timer_t *t = (lv_timer_t *)lv_event_get_user_data(e);
    if (t != NULL) {
        lv_timer_delete(t);
    }

    /* 真的离开录音机才停试听；转屏重建时当前页仍是录音机，要接着放 */
    if (app_navigator_current() != PAGE_RECORDER && playing_record()) {
        (void)srv_player_stop();
    }

    s_list     = NULL;
    s_time_lb  = NULL;
    s_state_lb = NULL;
    s_btn      = NULL;
    s_btn_lb   = NULL;
}

/* ===================== 页面 ===================== */

void ui_recorder_create(lv_obj_t *root)
{
    const lv_coord_t sw = ui_scr_w();
    const lv_coord_t sh = ui_scr_h();
    const bool relayout = app_navigator_in_page_rebuild();

    ui_titlebar_create(root, "录音机", app_navigator_back_smart_cb, NULL);

    /* 列表高度：横屏两行、竖屏三行；时间/状态/按钮往下排 */
    const lv_coord_t list_h = (sh >= 300) ? 96 : 58;

    s_list = lv_obj_create(root);
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_size(s_list, sw - 16, list_h);
    lv_obj_set_pos(s_list, 8, UI_TITLEBAR_H);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_radius(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_style_width(s_list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_list, UI_COLOR_TEXT_DIM, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_list, 2, LV_PART_SCROLLBAR);

    s_time_lb = lv_label_create(root);
    lv_obj_set_style_text_color(s_time_lb, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_time_lb, &font_clock_48, 0);
    lv_label_set_text(s_time_lb, "00:00");
    lv_obj_align(s_time_lb, LV_ALIGN_TOP_MID, 0, UI_TITLEBAR_H + list_h + 2);

    s_state_lb = lv_label_create(root);
    lv_obj_set_style_text_color(s_state_lb, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_state_lb, &font_ui_14, 0);
    /* 存的路径可能很长（rec-123.wav），限宽 + 单行滚动，别撑出屏幕 */
    lv_obj_set_width(s_state_lb, sw - 16);
    lv_obj_set_height(s_state_lb, font_ui_14.line_height);
    lv_obj_set_style_text_align(s_state_lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_state_lb, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(s_state_lb, 6000, 0);
    lv_label_set_text(s_state_lb,
                      srv_sd_is_mounted() ? "点上面的录音试听" : "SD 卡未挂载");
    lv_obj_align(s_state_lb, LV_ALIGN_TOP_MID, 0, UI_TITLEBAR_H + list_h + 64);

    s_btn = lv_button_create(root);
    lv_obj_set_size(s_btn, BTN_W, BTN_H);
    lv_obj_align(s_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_radius(s_btn, 10, 0);
    lv_obj_set_style_shadow_width(s_btn, 0, 0);
    lv_obj_set_style_pad_all(s_btn, 0, 0);
    lv_obj_set_style_bg_color(s_btn, UI_COLOR_PRIMARY, 0);
    ui_add_click_feedback(s_btn);
    lv_obj_add_event_cb(s_btn, on_btn, LV_EVENT_CLICKED, NULL);

    s_btn_lb = lv_label_create(s_btn);
    lv_obj_set_style_text_font(s_btn_lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_btn_lb, lv_color_white(), 0);
    lv_label_set_text(s_btn_lb, "开始录音");
    lv_obj_center(s_btn_lb);

    /* 转屏重建时列表内容与选中项都重新建；进入页面则清掉上次的选中 */
    if (!relayout) {
        s_sel = -1;
    }
    list_rebuild();
    rec_update();

    lv_timer_t *tmr = lv_timer_create(rec_tick_cb, TICK_MS, NULL);
    lv_obj_add_event_cb(root, rec_root_del_cb, LV_EVENT_DELETE, tmr);
}
