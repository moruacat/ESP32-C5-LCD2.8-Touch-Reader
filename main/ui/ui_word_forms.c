#include "ui_word_forms.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "ui_dialog.h"
#include "app_navigator.h"
#include "srv_dict.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define SEARCH_X   8
#define SEARCH_Y   (UI_TITLEBAR_H + 4)
#define SEARCH_W   (ui_scr_w() - SEARCH_X * 2)
#define SEARCH_H   32
#define BODY_X     8
#define BODY_Y     (SEARCH_Y + SEARCH_H + 6)
#define BODY_W     (ui_scr_w() - BODY_X * 2)
#define BODY_H     (ui_scr_h() - BODY_Y - 4)

static lv_obj_t *s_input_lb;    /*!< 搜索框里的文字 */
static lv_obj_t *s_body;        /*!< 结果滚动容器 */
static char      s_word[40];

/* 标签列宽要容得下「第三人称单数」（6 个汉字约 84px），值从它右边开始 */
#define FORM_KEY_W   92

static lv_obj_t *add_line(lv_obj_t *parent, const char *label, const char *value,
                          uint32_t color, lv_coord_t y)
{
    lv_obj_t *k = lv_label_create(parent);
    lv_obj_set_style_text_font(k, &font_ui_14, 0);
    lv_obj_set_style_text_color(k, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_width(k, FORM_KEY_W);
    lv_obj_set_height(k, font_ui_14.line_height);
    lv_label_set_long_mode(k, LV_LABEL_LONG_DOT);
    lv_label_set_text(k, label);
    lv_obj_set_pos(k, 0, y);

    lv_obj_t *v = lv_label_create(parent);
    lv_obj_set_style_text_font(v, &font_ui_14, 0);
    lv_obj_set_style_text_color(v, lv_color_hex(color), 0);
    lv_obj_set_width(v, BODY_W - FORM_KEY_W - 8);
    lv_obj_set_height(v, font_ui_14.line_height);
    lv_label_set_long_mode(v, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(v, 6000, 0);
    lv_label_set_text(v, value);
    lv_obj_set_pos(v, FORM_KEY_W + 8, y);
    return v;
}

static void show_result(void)
{
    lv_obj_clean(s_body);

    srv_dict_forms_t f;
    esp_err_t r = srv_dict_forms(s_word, &f);

    lv_obj_t *big = lv_label_create(s_body);
    lv_obj_set_style_text_font(big, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(big, UI_COLOR_TEXT, 0);
    lv_label_set_text(big, f.base[0] ? f.base : s_word);
    lv_obj_set_pos(big, 0, 0);

    /* 音标 + 释义（原形词条） */
    srv_dict_entry_t e;
    memset(&e, 0, sizeof(e));
    const bool has_entry = (srv_dict_lookup(f.base, &e) == ESP_OK);

    lv_coord_t y = 34;              /* montserrat_24 的行高约 30 */

    if (has_entry && e.phonetic[0] != '\0') {
        lv_obj_t *ph = lv_label_create(s_body);
        lv_obj_set_style_text_font(ph, &font_ipa_14, 0);
        lv_obj_set_style_text_color(ph, UI_COLOR_TEXT_DIM, 0);
        lv_label_set_text(ph, e.phonetic);
        lv_obj_set_pos(ph, 0, y);
        y += 22;
    }

    if (f.changed) {
        lv_obj_t *t = lv_label_create(s_body);
        lv_obj_set_style_text_font(t, &font_ui_14, 0);
        lv_obj_set_style_text_color(t, UI_COLOR_PRIMARY, 0);
        lv_label_set_text_fmt(t, "%s → %s", s_word, f.base);
        lv_obj_set_pos(t, 0, y);
        y += 24;
    }

    y += 8;

    if (r != ESP_OK || !f.found) {
        lv_obj_t *tip = lv_label_create(s_body);
        lv_obj_set_style_text_font(tip, &font_ui_14, 0);
        lv_obj_set_style_text_color(tip, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_width(tip, BODY_W);
        lv_label_set_long_mode(tip, LV_LABEL_LONG_WRAP);
        if (!srv_dict_variants_ready()) {
            /* 释义能查到说明 dict.dat 在，缺的是变形表这两个文件 */
            lv_label_set_text(tip,
                              "没找到变形表文件\n"
                              "把 dict/variants.dat 和 variants.idx\n"
                              "拷到 SD 卡的 /dict/ 目录");
            lv_obj_set_style_text_color(tip, UI_COLOR_WARN, 0);
        } else {
            lv_label_set_text(tip, "这个原形没有变形记录（只收录规则/常见变化）");
        }
        lv_obj_set_pos(tip, 0, y);
        y += 40;
    } else {
        static const struct {
            const char *name;
            size_t      off;
        } rows[] = {
            {"过去式",      offsetof(srv_dict_forms_t, past)},
            {"过去分词",    offsetof(srv_dict_forms_t, past_part)},
            {"现在分词",    offsetof(srv_dict_forms_t, ing)},
            {"第三人称单数", offsetof(srv_dict_forms_t, third)},
            {"复数",        offsetof(srv_dict_forms_t, plural)},
            {"比较级",      offsetof(srv_dict_forms_t, comp)},
            {"最高级",      offsetof(srv_dict_forms_t, sup)},
        };
        for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
            const char *val = (const char *)&f + rows[i].off;
            if (val[0] == '\0') {
                continue;
            }
            /* 逗号分隔的多个形式显示成 "a / b" */
            char pretty[64];
            size_t n = 0;
            for (const char *p = val; *p != '\0' && n + 4 < sizeof(pretty); p++) {
                if (*p == ',') {
                    pretty[n++] = ' ';
                    pretty[n++] = '/';
                    pretty[n++] = ' ';
                } else {
                    pretty[n++] = *p;
                }
            }
            pretty[n] = '\0';
            (void)add_line(s_body, rows[i].name, pretty, 0xE6E9EF, y);
            y += 24;
        }
    }

    if (has_entry && e.def[0] != '\0') {
        y += 6;
        lv_obj_t *def = lv_label_create(s_body);
        lv_obj_set_style_text_font(def, &font_ui_14, 0);
        lv_obj_set_style_text_color(def, UI_COLOR_TEXT, 0);
        lv_obj_set_width(def, BODY_W);
        lv_label_set_long_mode(def, LV_LABEL_LONG_WRAP);
        lv_label_set_text(def, e.def);
        lv_obj_set_pos(def, 0, y);
    }
}

static void on_input_done(const char *text, void *arg)
{
    (void)arg;
    if (text == NULL || text[0] == '\0') {
        return;
    }
    snprintf(s_word, sizeof(s_word), "%s", text);
    lv_label_set_text(s_input_lb, s_word);
    lv_obj_set_style_text_color(s_input_lb, UI_COLOR_TEXT, 0);
    show_result();
}

static void on_search_clicked(lv_event_t *e)
{
    (void)e;
    ui_text_input_show_init("查变形（如 ran / better）", sizeof(s_word) - 1,
                            s_word, on_input_done, NULL);
}

void ui_word_forms_create(lv_obj_t *root)
{
    ui_titlebar_create(root, "变形查询", app_navigator_back_cb, NULL);

    lv_obj_t *box = lv_button_create(root);
    lv_obj_set_size(box, SEARCH_W, SEARCH_H);
    lv_obj_set_pos(box, SEARCH_X, SEARCH_Y);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_set_style_bg_color(box, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_shadow_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    ui_add_click_feedback(box);
    lv_obj_add_event_cb(box, on_search_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ic = lv_image_create(box);
    lv_image_set_src(ic, &ic_search);
    lv_obj_set_style_image_recolor(ic, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_radius(ic, 0, 0);
    lv_image_set_scale(ic, 20 * 256 / 32);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 10, 0);

    s_input_lb = lv_label_create(box);
    lv_obj_set_style_text_color(s_input_lb, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_input_lb, &font_ui_14, 0);
    lv_obj_set_width(s_input_lb, SEARCH_W - 48);
    lv_label_set_long_mode(s_input_lb, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_input_lb, "点这里输入单词");
    lv_obj_align(s_input_lb, LV_ALIGN_LEFT_MID, 38, 0);

    s_body = lv_obj_create(root);
    lv_obj_set_size(s_body, BODY_W, BODY_H);
    lv_obj_set_pos(s_body, BODY_X, BODY_Y);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_body, 0, 0);
    lv_obj_set_style_radius(s_body, 0, 0);
    lv_obj_set_style_pad_all(s_body, 0, 0);
    lv_obj_set_scroll_dir(s_body, LV_DIR_VER);
    lv_obj_set_style_width(s_body, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_body, UI_COLOR_TEXT_DIM, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_body, 2, LV_PART_SCROLLBAR);

    if (s_word[0] != '\0') {
        lv_label_set_text(s_input_lb, s_word);
        lv_obj_set_style_text_color(s_input_lb, UI_COLOR_TEXT, 0);
        show_result();
    } else {
        lv_obj_t *tip = lv_label_create(s_body);
        lv_obj_set_style_text_font(tip, &font_ui_14, 0);
        lv_obj_set_style_text_color(tip, UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_width(tip, BODY_W);
        lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(tip, LV_LABEL_LONG_WRAP);
        lv_label_set_text(tip, "输入任意形式，例如\nran → run\nbetter → good\nstudies → study");
        lv_obj_align(tip, LV_ALIGN_CENTER, 0, 0);
    }
}
