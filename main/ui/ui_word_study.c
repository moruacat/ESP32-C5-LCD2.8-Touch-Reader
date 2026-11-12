#include "ui_word_study.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "app_navigator.h"
#include "srv_dict.h"
#include "srv_sd.h"
#include "srv_wordbook.h"

#include <stdio.h>
#include <string.h>

/* ===================== 布局 ===================== */

#define BODY_Y      UI_TITLEBAR_H
#define BODY_H      (UI_SCREEN_H - BODY_Y)

/* 下面这些都是相对选择/学习视图容器（容器顶在 BODY_Y）的坐标 */
#define LIST_X      8
#define LIST_Y      34
#define LIST_W      (UI_SCREEN_W - LIST_X * 2)
#define LIST_H      (BODY_H - LIST_Y - 46)
#define ROW_H       34

#define CARD_W      260
#define CARD_H      140
#define CARD_X      ((UI_SCREEN_W - CARD_W) / 2)
#define CARD_Y      24

#define SWIPE_TH    64          /*!< 超过这个距离松手才算数 */
#define SESS_WORDS  20          /*!< 每次随机抽多少个词 */

static lv_obj_t *s_root;
static lv_obj_t *s_pick;        /*!< 选择视图 */
static lv_obj_t *s_study;       /*!< 学习视图 */
static lv_obj_t *s_unit_list;
static lv_obj_t *s_book_lb;
static lv_obj_t *s_start_lb;
static lv_obj_t *s_wrong_lb;

static int  s_book = 0;
static bool s_sel[WB_MAX_UNITS];

/* 学习视图控件 */
static lv_obj_t *s_prog_lb;
static lv_obj_t *s_card;
static lv_obj_t *s_front;
static lv_obj_t *s_back;
static lv_obj_t *s_word_lb;
static lv_obj_t *s_phon_lb;
static lv_obj_t *s_def_lb;
static lv_obj_t *s_hint_lb;

/* 手势状态 */
static lv_point_t s_press;
static bool       s_tracking;
static bool       s_moved;
static bool       s_flipped;
static bool       s_flying;     /*!< 飞出动画进行中，忽略新的触摸 */
static bool       s_finished;   /*!< 本轮已结束，点提示可回选择页 */

/* 底部提示：本轮结束后点它回单元选择（to_pick 只有这里用到） */
static void to_pick(void);

static void on_hint_click(lv_event_t *e)
{
    (void)e;
    if (s_finished) {
        to_pick();
    }
}

/* ===================== 选择视图 ===================== */

static int sel_count(void)
{
    int n = 0;
    const int cnt = srv_wordbook_unit_count(s_book);
    for (int i = 0; i < cnt; i++) {
        if (s_sel[i]) {
            n++;
        }
    }
    return n;
}

static void update_start_text(void)
{
    if (s_start_lb == NULL) {
        return;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "开始学习 (%d)", sel_count());
    lv_label_set_text(s_start_lb, buf);
}

static void unit_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= srv_wordbook_unit_count(s_book)) {
        return;
    }
    s_sel[idx] = !s_sel[idx];
    lv_obj_t *row = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_set_style_bg_color(row, s_sel[idx] ? UI_COLOR_PRIMARY : UI_COLOR_SURFACE, 0);
    update_start_text();
}

static void pick_rebuild(void)
{
    if (s_unit_list == NULL) {
        return;
    }
    lv_obj_clean(s_unit_list);

    const int cnt = srv_wordbook_unit_count(s_book);
    for (int i = 0; i < cnt; i++) {
        lv_obj_t *row = lv_obj_create(s_unit_list);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(row, LIST_W - 8, ROW_H - 4);
        lv_obj_set_pos(row, 0, (lv_coord_t)(i * ROW_H));
        lv_obj_set_style_bg_color(row, s_sel[i] ? UI_COLOR_PRIMARY : UI_COLOR_SURFACE, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        ui_add_click_feedback(row);
        lv_obj_add_event_cb(row, unit_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *name = lv_label_create(row);
        lv_obj_set_style_text_font(name, &font_ui_14, 0);
        lv_obj_set_style_text_color(name, UI_COLOR_TEXT, 0);
        lv_label_set_text(name, srv_wordbook_unit_name(s_book, i));
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 10, 0);

        char buf[24];
        snprintf(buf, sizeof(buf), "%d 词", srv_wordbook_unit_words(s_book, i));
        lv_obj_t *cnt_lb = lv_label_create(row);
        lv_obj_set_style_text_font(cnt_lb, &font_ui_14, 0);
        lv_obj_set_style_text_color(cnt_lb, UI_COLOR_TEXT_DIM, 0);
        lv_label_set_text(cnt_lb, buf);
        lv_obj_align(cnt_lb, LV_ALIGN_RIGHT_MID, -10, 0);
    }

    if (s_book_lb != NULL) {
        lv_label_set_text(s_book_lb, srv_wordbook_book_name(s_book));
    }
    if (s_wrong_lb != NULL) {
        char buf[32];
        snprintf(buf, sizeof(buf), "错词本 (%d)", srv_wordbook_wrong_count());
        lv_label_set_text(s_wrong_lb, buf);
    }
    update_start_text();
}

static void to_study(void);
/* to_pick 的前向声明在手势状态区（on_hint_click 要用） */

static void on_start_clicked(lv_event_t *e)
{
    (void)e;
    if (sel_count() == 0) {
        return;
    }
    if (srv_wordbook_start_units(s_book, s_sel, SESS_WORDS) == ESP_OK) {
        to_study();
    }
}

static void on_wrong_clicked(lv_event_t *e)
{
    (void)e;
    if (srv_wordbook_start_wrong(SESS_WORDS) == ESP_OK) {
        to_study();
    }
}

static void on_book_prev(lv_event_t *e)
{
    (void)e;
    if (srv_wordbook_book_count() <= 0) {
        return;
    }
    s_book = (s_book + srv_wordbook_book_count() - 1) % srv_wordbook_book_count();
    memset(s_sel, 0, sizeof(s_sel));
    pick_rebuild();
}

static void on_book_next(lv_event_t *e)
{
    (void)e;
    if (srv_wordbook_book_count() <= 0) {
        return;
    }
    s_book = (s_book + 1) % srv_wordbook_book_count();
    memset(s_sel, 0, sizeof(s_sel));
    pick_rebuild();
}

static void pick_build(lv_obj_t *parent)
{
    /* 词书切换行 */
    lv_obj_t *prev = lv_button_create(parent);
    lv_obj_set_size(prev, 28, 24);
    lv_obj_set_pos(prev, 8, 4);
    lv_obj_set_style_radius(prev, 6, 0);
    lv_obj_set_style_bg_color(prev, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_shadow_width(prev, 0, 0);
    lv_obj_set_style_pad_all(prev, 0, 0);
    ui_add_click_feedback(prev);
    lv_obj_add_event_cb(prev, on_book_prev, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pic = lv_image_create(prev);
    lv_image_set_src(pic, &ic_arrow_left);
    lv_obj_set_style_image_recolor(pic, UI_COLOR_TEXT, 0);
    lv_obj_center(pic);

    s_book_lb = lv_label_create(parent);
    lv_obj_set_style_text_font(s_book_lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_book_lb, UI_COLOR_TEXT, 0);
    lv_obj_set_width(s_book_lb, 200);
    lv_obj_set_style_text_align(s_book_lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_book_lb, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(s_book_lb, 6000, 0);
    lv_obj_set_pos(s_book_lb, 60, 7);

    lv_obj_t *next = lv_button_create(parent);
    lv_obj_set_size(next, 28, 24);
    lv_obj_set_pos(next, UI_SCREEN_W - 36, 4);
    lv_obj_set_style_radius(next, 6, 0);
    lv_obj_set_style_bg_color(next, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_shadow_width(next, 0, 0);
    lv_obj_set_style_pad_all(next, 0, 0);
    ui_add_click_feedback(next);
    lv_obj_add_event_cb(next, on_book_next, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nic = lv_image_create(next);
    lv_image_set_src(nic, &ic_arrow_right);
    lv_obj_set_style_image_recolor(nic, UI_COLOR_TEXT, 0);
    lv_obj_center(nic);

    /* 单元列表 */
    s_unit_list = lv_obj_create(parent);
    lv_obj_set_size(s_unit_list, LIST_W, LIST_H);
    lv_obj_set_pos(s_unit_list, LIST_X, LIST_Y);
    lv_obj_set_style_bg_opa(s_unit_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_unit_list, 0, 0);
    lv_obj_set_style_radius(s_unit_list, 0, 0);
    lv_obj_set_style_pad_all(s_unit_list, 0, 0);
    lv_obj_set_scroll_dir(s_unit_list, LV_DIR_VER);
    lv_obj_set_style_width(s_unit_list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_unit_list, UI_COLOR_TEXT_DIM, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_unit_list, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_unit_list, 2, LV_PART_SCROLLBAR);

    /* 底部按钮 */
    lv_obj_t *wrong = lv_button_create(parent);
    lv_obj_set_size(wrong, 140, 34);
    lv_obj_set_pos(wrong, 8, BODY_H - 42);
    lv_obj_set_style_radius(wrong, 8, 0);
    lv_obj_set_style_bg_color(wrong, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_shadow_width(wrong, 0, 0);
    lv_obj_set_style_pad_all(wrong, 0, 0);
    ui_add_click_feedback(wrong);
    lv_obj_add_event_cb(wrong, on_wrong_clicked, LV_EVENT_CLICKED, NULL);
    s_wrong_lb = lv_label_create(wrong);
    lv_obj_set_style_text_font(s_wrong_lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_wrong_lb, UI_COLOR_TEXT, 0);
    lv_label_set_text(s_wrong_lb, "错词本");
    lv_obj_center(s_wrong_lb);

    lv_obj_t *start = lv_button_create(parent);
    lv_obj_set_size(start, 156, 34);
    lv_obj_set_pos(start, UI_SCREEN_W - 164, BODY_H - 42);
    lv_obj_set_style_radius(start, 8, 0);
    lv_obj_set_style_bg_color(start, UI_COLOR_PRIMARY, 0);
    lv_obj_set_style_shadow_width(start, 0, 0);
    lv_obj_set_style_pad_all(start, 0, 0);
    ui_add_click_feedback(start);
    lv_obj_add_event_cb(start, on_start_clicked, LV_EVENT_CLICKED, NULL);
    s_start_lb = lv_label_create(start);
    lv_obj_set_style_text_font(s_start_lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_start_lb, lv_color_white(), 0);
    lv_obj_center(s_start_lb);

    pick_rebuild();
}

/* ===================== 学习视图 ===================== */

static void card_show(void);

static void fly_done_cb(lv_anim_t *a)
{
    (void)a;
    lv_obj_set_style_translate_x(s_card, 0, 0);
    lv_obj_set_style_bg_color(s_card, UI_COLOR_SURFACE, 0);
    s_flying = false;
    card_show();
}

static void anim_x_cb(void *var, int32_t v)
{
    lv_obj_set_style_translate_x((lv_obj_t *)var, (lv_coord_t)v, 0);
}

static void card_slide_back(void)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_card);
    lv_anim_set_exec_cb(&a, anim_x_cb);
    lv_anim_set_values(&a, (int32_t)lv_obj_get_style_translate_x(s_card, 0), 0);
    lv_anim_set_duration(&a, 160);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    lv_obj_set_style_bg_color(s_card, UI_COLOR_SURFACE, 0);
}

static void card_fly_out(int dir, bool known)
{
    s_flying = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_card);
    lv_anim_set_exec_cb(&a, anim_x_cb);
    lv_anim_set_values(&a, (int32_t)lv_obj_get_style_translate_x(s_card, 0), dir * 380);
    lv_anim_set_duration(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, fly_done_cb);
    lv_anim_start(&a);

    /* 答案先记下，飞出回调里再取下一张 */
    srv_wordbook_answer(known);
}

static void set_front(bool front)
{
    s_flipped = !front;
    if (front) {
        lv_obj_clear_flag(s_front, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_front, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    }
}

static void card_show(void)
{
    const char *w = srv_wordbook_current();
    if (w[0] == '\0') {
        /* 会话结束：卡片区换成统计，点提示回选择页 */
        s_finished = true;
        lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
        const int total = srv_wordbook_session_total();
        const int known = srv_wordbook_session_known();
        char buf[96];
        snprintf(buf, sizeof(buf), "本轮结束\n认识 %d / %d\n不认识 %d",
                 known, total, total - known);
        lv_obj_set_style_text_font(s_prog_lb, &font_ui_14, 0);
        lv_obj_set_style_text_color(s_prog_lb, UI_COLOR_TEXT, 0);
        lv_label_set_text(s_prog_lb, buf);
        lv_obj_align(s_prog_lb, LV_ALIGN_CENTER, 0, -10);
        lv_label_set_text(s_hint_lb, "点这里返回选择");
        return;
    }

    s_finished = false;

    char buf[32];
    snprintf(buf, sizeof(buf), "%d / %d",
             srv_wordbook_session_done() + 1, srv_wordbook_session_total());
    /* 从结束态重新开始学习时，把进度行恢复成顶部小字 */
    lv_obj_set_style_text_font(s_prog_lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_prog_lb, UI_COLOR_TEXT_DIM, 0);
    lv_obj_align(s_prog_lb, LV_ALIGN_TOP_MID, 0, 4);
    lv_label_set_text(s_prog_lb, buf);

    lv_label_set_text(s_word_lb, w);

    srv_dict_entry_t e;
    memset(&e, 0, sizeof(e));
    if (srv_dict_lookup_lemma(w, &e) == ESP_OK) {
        lv_label_set_text(s_phon_lb, e.phonetic);
        lv_label_set_text(s_def_lb, (e.def[0] != '\0') ? e.def : "（词典没有释义）");
    } else {
        lv_label_set_text(s_phon_lb, "");
        lv_label_set_text(s_def_lb, "（词典未收录）");
    }

    set_front(true);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_translate_x(s_card, 0, 0);
    lv_obj_set_style_bg_color(s_card, UI_COLOR_SURFACE, 0);
    lv_label_set_text(s_hint_lb, "点卡片看释义    ← 认识    不认识 →");
}

/* ---- 卡片手势 ---- */

static void card_pressed(lv_event_t *e)
{
    (void)e;
    if (s_flying) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if (indev != NULL) {
        lv_indev_get_point(indev, &s_press);
        s_tracking = true;
        s_moved = false;
    }
}

static void card_pressing(lv_event_t *e)
{
    (void)e;
    if (!s_tracking || s_flying) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int dx = (int)p.x - (int)s_press.x;

    if (dx > 4 || dx < -4) {
        s_moved = true;
    }
    lv_obj_set_style_translate_x(s_card, (lv_coord_t)dx, 0);

    /* 距离越远颜色越深：左绿（认识）/ 右红（不认识） */
    int dist = (dx < 0) ? -dx : dx;
    uint8_t mix = (uint8_t)((dist >= SWIPE_TH) ? 255 : dist * 255 / SWIPE_TH);
    lv_color_t base = (dx < 0) ? UI_COLOR_OK : UI_COLOR_TEMP;
    lv_obj_set_style_bg_color(s_card, lv_color_mix(base, UI_COLOR_SURFACE, mix), 0);
}

static void card_released(lv_event_t *e)
{
    (void)e;
    if (!s_tracking) {
        return;
    }
    s_tracking = false;
    if (s_flying) {
        return;
    }

    int dx = (int)lv_obj_get_style_translate_x(s_card, 0);

    if (dx <= -SWIPE_TH) {
        card_fly_out(-1, true);         /* 左滑：认识 */
        return;
    }
    if (dx >= SWIPE_TH) {
        card_fly_out(1, false);         /* 右滑：不认识 */
        return;
    }

    card_slide_back();
    if (!s_moved) {
        /* s_flipped 是「当前在背面」：目标是它的反面，正好等于当前值取反后的
         * front 参数（false=背面），所以直接传 s_flipped 即可 */
        set_front(s_flipped);
    }
}

static void card_press_lost(lv_event_t *e)
{
    (void)e;
    if (s_tracking) {
        s_tracking = false;
        card_slide_back();
    }
}

static void study_build(lv_obj_t *parent)
{
    s_prog_lb = lv_label_create(parent);
    lv_obj_set_style_text_font(s_prog_lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_prog_lb, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(s_prog_lb, "");
    lv_obj_align(s_prog_lb, LV_ALIGN_TOP_MID, 0, 4);

    s_card = lv_obj_create(parent);
    lv_obj_set_size(s_card, CARD_W, CARD_H);
    lv_obj_set_pos(s_card, CARD_X, CARD_Y);
    lv_obj_set_style_bg_color(s_card, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_card, 0, 0);
    lv_obj_set_style_radius(s_card, 12, 0);
    lv_obj_set_style_pad_all(s_card, 0, 0);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_card, card_pressed,    LV_EVENT_PRESSED,    NULL);
    lv_obj_add_event_cb(s_card, card_pressing,   LV_EVENT_PRESSING,   NULL);
    lv_obj_add_event_cb(s_card, card_released,   LV_EVENT_RELEASED,   NULL);
    lv_obj_add_event_cb(s_card, card_press_lost, LV_EVENT_PRESS_LOST, NULL);

    /* 正面：单词 + 音标（子容器不接收点击，让手势都落到卡片上） */
    s_front = lv_obj_create(s_card);
    lv_obj_remove_flag(s_front, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_front, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_front, CARD_W, CARD_H);
    lv_obj_set_pos(s_front, 0, 0);
    lv_obj_set_style_bg_opa(s_front, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_front, 0, 0);
    lv_obj_set_style_pad_all(s_front, 0, 0);

    s_word_lb = lv_label_create(s_front);
    lv_obj_set_style_text_font(s_word_lb, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_word_lb, UI_COLOR_TEXT, 0);
    lv_obj_set_width(s_word_lb, CARD_W - 24);
    lv_obj_set_style_text_align(s_word_lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_word_lb, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(s_word_lb, 6000, 0);
    lv_label_set_text(s_word_lb, "");
    lv_obj_align(s_word_lb, LV_ALIGN_CENTER, 0, -12);

    s_phon_lb = lv_label_create(s_front);
    lv_obj_set_style_text_font(s_phon_lb, &font_ipa_14, 0);
    lv_obj_set_style_text_color(s_phon_lb, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_width(s_phon_lb, CARD_W - 24);
    lv_obj_set_style_text_align(s_phon_lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_phon_lb, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(s_phon_lb, "");
    lv_obj_align(s_phon_lb, LV_ALIGN_CENTER, 0, 24);

    /* 背面：释义 */
    s_back = lv_obj_create(s_card);
    lv_obj_remove_flag(s_back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_back, CARD_W, CARD_H);
    lv_obj_set_pos(s_back, 0, 0);
    lv_obj_set_style_bg_opa(s_back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_back, 0, 0);
    lv_obj_set_style_pad_all(s_back, 0, 0);
    lv_obj_add_flag(s_back, LV_OBJ_FLAG_HIDDEN);

    s_def_lb = lv_label_create(s_back);
    lv_obj_set_style_text_font(s_def_lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_def_lb, UI_COLOR_TEXT, 0);
    lv_obj_set_width(s_def_lb, CARD_W - 24);
    lv_obj_set_height(s_def_lb, CARD_H - 16);
    lv_obj_set_style_text_align(s_def_lb, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(s_def_lb, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_def_lb, "");
    lv_obj_center(s_def_lb);

    s_hint_lb = lv_label_create(parent);
    lv_obj_set_style_text_font(s_hint_lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_hint_lb, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_width(s_hint_lb, UI_SCREEN_W - 16);
    lv_obj_set_style_text_align(s_hint_lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_height(s_hint_lb, font_ui_14.line_height);
    lv_label_set_long_mode(s_hint_lb, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(s_hint_lb, "");
    lv_obj_align(s_hint_lb, LV_ALIGN_BOTTOM_MID, 0, -14);
    /* 本轮结束后点这里回单元选择 */
    lv_obj_add_flag(s_hint_lb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_hint_lb, on_hint_click, LV_EVENT_CLICKED, NULL);
}

/* ===================== 视图切换 ===================== */

static void to_study(void)
{
    if (s_pick != NULL) {
        lv_obj_add_flag(s_pick, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_study == NULL) {
        s_study = lv_obj_create(s_root);
        lv_obj_remove_flag(s_study, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(s_study, UI_SCREEN_W, BODY_H);
        lv_obj_set_pos(s_study, 0, BODY_Y);
        lv_obj_set_style_bg_color(s_study, UI_COLOR_BG, 0);
        lv_obj_set_style_bg_opa(s_study, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_study, 0, 0);
        lv_obj_set_style_radius(s_study, 0, 0);
        lv_obj_set_style_pad_all(s_study, 0, 0);
        study_build(s_study);
    }
    lv_obj_clear_flag(s_study, LV_OBJ_FLAG_HIDDEN);
    card_show();
}

static void to_pick(void)
{
    srv_wordbook_stop();
    s_finished = false;
    if (s_study != NULL) {
        lv_obj_add_flag(s_study, LV_OBJ_FLAG_HIDDEN);
        s_flying = false;
        s_tracking = false;
    }
    if (s_pick != NULL) {
        lv_obj_clear_flag(s_pick, LV_OBJ_FLAG_HIDDEN);
        pick_rebuild();
    }
}

/* ===================== 页面 ===================== */

static void study_root_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    srv_wordbook_stop();

    s_root      = NULL;
    s_pick      = NULL;
    s_study     = NULL;
    s_unit_list = NULL;
    s_book_lb   = NULL;
    s_start_lb  = NULL;
    s_wrong_lb  = NULL;
    s_prog_lb   = NULL;
    s_card      = NULL;
    s_front     = NULL;
    s_back      = NULL;
    s_word_lb   = NULL;
    s_phon_lb   = NULL;
    s_def_lb    = NULL;
    s_hint_lb   = NULL;
}

void ui_word_study_create(lv_obj_t *root)
{
    s_root = root;
    ui_titlebar_create(root, "单词学习", app_navigator_back_smart_cb, NULL);
    lv_obj_add_event_cb(root, study_root_del_cb, LV_EVENT_DELETE, NULL);

    memset(s_sel, 0, sizeof(s_sel));
    s_book = 0;

    if (srv_sd_is_mounted()) {
        srv_wordbook_reload();      /* 每次进页面重扫，词书可能刚拷进来 */
    }

    if (!srv_sd_is_mounted() || srv_wordbook_book_count() == 0) {
        lv_obj_t *tip = lv_label_create(root);
        lv_obj_set_style_text_font(tip, &font_ui_14, 0);
        lv_obj_set_style_text_color(tip, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(tip, UI_SCREEN_W - 32);
        lv_label_set_long_mode(tip, LV_LABEL_LONG_WRAP);
        lv_label_set_text(tip,
                          "SD 卡 /words 下还没有词书\n\n"
                          "用 tools/build_wordbook.py 分析课文/词表，\n"
                          "把生成的目录拷到 /words/ 即可");
        lv_obj_align(tip, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    s_pick = lv_obj_create(root);
    lv_obj_remove_flag(s_pick, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_pick, UI_SCREEN_W, BODY_H);
    lv_obj_set_pos(s_pick, 0, BODY_Y);
    lv_obj_set_style_bg_color(s_pick, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_pick, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pick, 0, 0);
    lv_obj_set_style_radius(s_pick, 0, 0);
    lv_obj_set_style_pad_all(s_pick, 0, 0);
    pick_build(s_pick);
}
