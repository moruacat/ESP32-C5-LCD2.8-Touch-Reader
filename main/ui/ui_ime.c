/*
 * 中文输入层：ui_dialog.c 里那套「标题 + 取消 + 输入框 + 屏幕键盘」的拼音版。
 *
 * 整层的骨架、配色、尺寸都照着 ui_text_input_show() 抄，只把底下的键盘从
 * LVGL 自带的 lv_keyboard 换成 components/lvgl_pinyin_ime 的拼音键盘，这样
 * 两个输入层的观感一致，调用方换过去也不用改任何别的东西。
 *
 * 组件只编进固件（sim/CMakeLists.txt 只收 main/ui、main/services 那几个目录，
 * 不含这个组件），所以这里不直接 include 它的头，改成用 __has_include 探：
 * 固件里 main/CMakeLists.txt 的 REQUIRES 带上了 lvgl_pinyin_ime，头找得到，
 * 走拼音键盘；找不到（模拟器）就整体退回英文键盘。
 */

#include "ui_ime.h"

#include "ui_common.h"
#include "ui_dialog.h"      /* ui_alert_close() / 退化时的 ui_text_input_show() */

#include "esp_log.h"
#include "esp_heap_caps.h"  /* 自检里要问「最大连续内存还有多大」 */
#include "srv_sd.h"         /* 自检里要判 SD 卡挂没挂上 */
#include "lvgl.h"

#include <stdio.h>

#if defined(__has_include)
#  if __has_include("lvgl_pinyin_ime/lv_pinyin_ime.h")
#    include "lvgl_pinyin_ime/lv_pinyin_ime.h"
#    define UI_IME_HAVE_PINYIN  1
#  endif
#endif

#ifndef UI_IME_HAVE_PINYIN
#  define UI_IME_HAVE_PINYIN  0
#endif

#if !UI_IME_HAVE_PINYIN

/* ===================== 没有拼音组件：借用英文键盘那一层 ===================== */

static const char *TAG = "ui_ime";

void ui_ime_diag_log(void)
{
    ESP_LOGW(TAG, "输入法：这个构建没编入拼音组件，所有输入都走英文键盘");
}

void ui_ime_text_input_show(const char *title, uint32_t max_len,
                            ui_text_input_cb_t cb, void *arg)
{
    ui_text_input_show(title, max_len, cb, arg);
}

#else

/* ===================== 拼音键盘 ===================== */

static const char *TAG = "ui_ime";

/*! 键盘区高度：与英文键盘同一档，240 高的屏上正好占下半屏 */
#define UI_IME_KB_H      160
/*! 输入框内容上限（缓冲区的栈开销在 LVGL 任务上，跟英文那一层取同一个数） */
#define UI_IME_TEXT_MAX  128

/* ===================== 开机自检 ===================== */

/**
 * @brief 读一个文件的大小；打不开（卡不在 / 文件没拷）就返回 0
 */
static size_t ime_file_size(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return 0;
    }

    size_t size = 0;
    if (fseek(fp, 0, SEEK_END) == 0) {
        long n = ftell(fp);
        size = (n > 0) ? (size_t)n : 0;
    }
    fclose(fp);
    return size;
}

/**
 * @brief 这么大的字体文件要占多少内存
 *
 * 和组件里 ime_font_ram_need()（components/lvgl_pinyin_ime/src/data/ime_font.c）
 * 是同一条规则：字形位图整表拷进 RAM，再加每个字形一个描述符，大约是
 * 「文件大小 + 1/4 + 96 KB 富余」。这里只是把这条规则抄来算给日志看，
 * 真正挑档位的还是组件自己（它会按同一套规则逐档往下试）。
 */
static size_t ime_font_need(size_t file_size)
{
    return file_size + file_size / 4 + (96 * 1024);
}

/**
 * @brief 开机自检：把「输入法为什么没出来」提前打出来
 *
 * 只查料齐不齐、内存够不够，**不加载字体** —— 字体真加载要几 MB 连续内存，
 * 不能为了看一眼就白占着（第一次点开输入法时组件才加载，那几行日志也是现成的）。
 */
void ui_ime_diag_log(void)
{
    /* 文件和 sdkconfig 里 CONFIG_LV_PINYIN_IME_*_SD_PATH 一一对应，改配置这里要跟着改 */
    static const char *P_DICT  = "/sdcard/dict_pinyin.dat";
    static const char *P_BIG   = "/sdcard/ime_font_big.bin";
    static const char *P_SAFE  = "/sdcard/ime_font_big_safe.bin";
    static const char *P_SMALL = "/sdcard/ime_font_small.bin";

    const size_t dict  = ime_file_size(P_DICT);
    const size_t big   = ime_file_size(P_BIG);
    const size_t safe  = ime_file_size(P_SAFE);
    const size_t small = ime_file_size(P_SMALL);

    /* 组件挑档位问的是「最大连续块」，不是空闲总量：字体得要一整块 */
    const size_t avail = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

    ESP_LOGI(TAG, "输入法自检：SD %s，词库 %u KB，字库 大 %u KB / 备 %u KB / 小 %u KB，"
                  "开机时最大连续块 %u KB",
             srv_sd_is_mounted() ? "已挂载" : "未挂载",
             (unsigned)(dict / 1024), (unsigned)(big / 1024),
             (unsigned)(safe / 1024), (unsigned)(small / 1024),
             (unsigned)(avail / 1024));

    if (dict == 0) {
        ESP_LOGW(TAG, "输入法：没读到 %s，拼音引擎起不来，会退成英文键盘", P_DICT);
        return;
    }

    if (big > 0 && avail >= ime_font_need(big)) {
        ESP_LOGI(TAG, "输入法：按开机时的空闲内存看，词库和主字库（要 %u KB）都放得下",
                 (unsigned)(ime_font_need(big) / 1024));
    } else if (safe > 0 && avail >= ime_font_need(safe)) {
        ESP_LOGI(TAG, "输入法：主字库要 %u KB 连续内存、开机时只有 %u KB，先退用备档（要 %u KB）",
                 (unsigned)(ime_font_need(big) / 1024), (unsigned)(avail / 1024),
                 (unsigned)(ime_font_need(safe) / 1024));
    } else {
        ESP_LOGW(TAG, "输入法：字库都放不下（主字库要 %u KB、备档要 %u KB，开机时只有 %u KB），"
                      "会退成英文键盘",
                 (unsigned)(ime_font_need(big) / 1024),
                 (unsigned)(ime_font_need(safe) / 1024),
                 (unsigned)(avail / 1024));
    }

    if (big > 0 && small == 0) {
        /* 小字库只画按键说明（中英/空格那几行），缺了不挡打字 */
        ESP_LOGW(TAG, "输入法：卡上没读到 %s，打字不受影响，但那几行按键说明会画成方框", P_SMALL);
    }

    /* 内存这栏开机之后还会变（WiFi 缓冲、电台预蓄水都在抢），所以上面只是预估：
     * 真正的加载在第一次弹出输入法时做，以那时 ime_dict / ime_font 打的两行日志为准。 */
    ESP_LOGI(TAG, "输入法：真正加载在第一次弹输入法时，以那一刻 ime_dict / ime_font 的日志为准");
}

static ui_text_input_cb_t s_ime_cb;
static void              *s_ime_arg;
static lv_obj_t          *s_ime_mask;
static lv_obj_t          *s_ime_ta;
static lv_obj_t          *s_ime_kb;

static void ime_close(void)
{
    if (s_ime_mask != NULL && lv_obj_is_valid(s_ime_mask)) {
        lv_obj_delete(s_ime_mask);      /* 触发 del 回调，清掉下面几个 static */
    }
}

static void ime_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    if (lv_event_get_target(e) != s_ime_mask) {
        return;                         /* 已经是新的输入层了，别清 */
    }
    s_ime_mask = NULL;
    s_ime_ta   = NULL;
    s_ime_kb   = NULL;
    s_ime_cb   = NULL;
    s_ime_arg  = NULL;

    /* 弹层没了：把输入法字体占的几 MB 连续内存还给后台音乐/电台 */
    lv_pinyin_ime_font_release();
}

static void ime_cancel_cb(lv_event_t *e)
{
    (void)e;
    ime_close();
}

/* 键盘上的「确定」键：缓冲区空的时候组件把事件发到键盘对象上 */
static void ime_ready_cb(lv_event_t *e)
{
    (void)e;

    /* 先把文字抄出来：弹层一关输入框就没了 */
    char text[UI_IME_TEXT_MAX + 1];
    text[0] = '\0';
    if (s_ime_ta != NULL) {
        snprintf(text, sizeof(text), "%s", lv_textarea_get_text(s_ime_ta));
    }

    ui_text_input_cb_t cb = s_ime_cb;
    void *arg = s_ime_arg;

    ime_close();
    if (cb != NULL) {
        cb(text, arg);
    }
}

/* 兜底：在空白处点一下，把键盘重新顶上来。
 *
 * 只兜「点在没有子控件的 mask 区域」这一种情况：LVGL 9 的事件默认不冒泡
 * （LV_OBJ_FLAG_EVENT_BUBBLE 谁都没开），点顶栏、点输入框的点击都停在各自控件上，
 * 走不到这里；组件自带的「点空白收起键盘」绑在 screen 上，而本层的 mask 铺满整屏、
 * 又把点击吃掉了，它同样收不到 —— 所以这个输入层里键盘是一直留着的，整层只由
 * 「取消 / 确定」关掉。 */
static void ime_show_cb(lv_event_t *e)
{
    (void)e;
    if (s_ime_kb != NULL) {
        lv_pinyin_ime_show(s_ime_kb);
    }
}

/* 弹层底板：整屏大小、能吃点击，挡住下面页面的点击，也当键盘的贴底层。
 * 比通用弹层更暗一点：底下压着结构复杂的键盘，太亮会抢注意力 */
static lv_obj_t *ime_mask_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    if (scr == NULL) {
        return NULL;
    }

    lv_obj_t *mask = lv_obj_create(scr);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(mask, ui_scr_w(), ui_scr_h());
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, 0);    /* 底下压着整块键盘，太亮会抢注意力 */
    lv_obj_set_style_border_width(mask, 0, 0);
    lv_obj_set_style_radius(mask, 0, 0);
    lv_obj_set_style_pad_all(mask, 0, 0);
    return mask;
}

/* 顶部面板：标题 + 取消 + 输入框，尺寸配色与 ui_dialog.c 的文字输入层逐项对齐 */
static void ime_panel_create(lv_obj_t *mask, const char *title, uint32_t max_len,
                             lv_obj_t **ta_out)
{
    lv_obj_t *box = lv_obj_create(mask);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(box, ui_scr_w() - 16, 66);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_bg_color(box, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 0, 0);

    /* 标题限宽 + 收省略号：不限宽的长标题会从中间被切掉，看着像乱码 */
    lv_obj_t *ttl = lv_label_create(box);
    lv_obj_set_style_text_font(ttl, &font_ui_14, 0);
    lv_obj_set_style_text_color(ttl, UI_COLOR_TEXT, 0);
    lv_label_set_text(ttl, (title != NULL) ? title : "");
    lv_obj_set_width(ttl, lv_obj_get_width(box) - 76);
    lv_label_set_long_mode(ttl, LV_LABEL_LONG_DOT);
    lv_obj_align(ttl, LV_ALIGN_TOP_LEFT, 10, 6);

    lv_obj_t *cancel = lv_button_create(box);
    lv_obj_set_size(cancel, 52, 24);
    lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -8, 5);
    lv_obj_set_style_radius(cancel, 6, 0);
    lv_obj_set_style_bg_color(cancel, UI_COLOR_BG, 0);
    lv_obj_set_style_shadow_width(cancel, 0, 0);
    lv_obj_set_style_pad_all(cancel, 0, 0);
    ui_add_click_feedback(cancel);
    lv_obj_add_event_cb(cancel, ime_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *clb = lv_label_create(cancel);
    lv_obj_set_style_text_font(clb, &font_ui_14, 0);
    lv_obj_set_style_text_color(clb, UI_COLOR_TEXT, 0);
    lv_label_set_text(clb, "取消");
    lv_obj_center(clb);

    lv_obj_t *ta = lv_textarea_create(box);
    lv_textarea_set_one_line(ta, true);         /* 单行：不打回车，只滚动显示 */
    lv_textarea_set_max_length(ta, max_len);
    lv_obj_set_size(ta, ui_scr_w() - 16 - 20, 26);
    lv_obj_align(ta, LV_ALIGN_BOTTOM_LEFT, 10, -7);
    lv_obj_set_style_bg_color(ta, UI_COLOR_BG, 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_radius(ta, 6, 0);
    lv_obj_set_style_text_font(ta, &font_ui_14, 0);
    lv_obj_set_style_text_color(ta, UI_COLOR_TEXT, 0);
    lv_obj_set_style_pad_all(ta, 3, 0);
    /* 拼音键盘不会往上抛 READY/CANCEL，留着是给物理键盘/别的路径兜底 */
    lv_obj_add_event_cb(ta, ime_ready_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(ta, ime_cancel_cb, LV_EVENT_CANCEL, NULL);

    *ta_out = ta;
}

/********************** 加载提示 **********************/

static lv_obj_t *s_load_hint;

/* 字体加载是同步阻塞的（分区/SD 都要读 1~3 MB），先把提示画出来再加载，
 * 否则用户点开输入法后会对着旧画面卡住好几秒，看起来像死机。 */
static void load_hint_show(lv_obj_t *mask)
{
    s_load_hint = lv_label_create(mask);
    lv_label_set_text(s_load_hint, "正在加载输入法…");
    lv_obj_set_style_text_font(s_load_hint, &font_ui_14, 0);
    lv_obj_set_style_text_color(s_load_hint, UI_COLOR_TEXT, 0);
    lv_obj_set_style_bg_color(s_load_hint, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_load_hint, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_load_hint, 8, 0);
    lv_obj_set_style_pad_hor(s_load_hint, 12, 0);
    lv_obj_set_style_pad_ver(s_load_hint, 6, 0);
    lv_obj_center(s_load_hint);
    lv_refr_now(NULL);          /* 先刷一帧，下面就是几秒的同步加载 */
}

static void load_hint_hide(void)
{
    if (s_load_hint != NULL && lv_obj_is_valid(s_load_hint)) {
        lv_obj_delete(s_load_hint);
    }
    s_load_hint = NULL;
}

void ui_ime_text_input_show(const char *title, uint32_t max_len,
                            ui_text_input_cb_t cb, void *arg)
{
    /* 同时只留一个弹层 */
    ui_alert_close();
    ime_close();

    if (max_len == 0 || max_len > UI_IME_TEXT_MAX) {
        max_len = UI_IME_TEXT_MAX;
    }

    /* 键盘高度是「建控件时读的全局状态」，必须在 create 之前设好 */
    lv_pinyin_ime_set_fit_height(UI_IME_KB_H);

    lv_obj_t *mask = ime_mask_create();
    if (mask == NULL) {
        return;
    }

    lv_obj_t *ta = NULL;
    ime_panel_create(mask, title, max_len, &ta);

    /* lv_pinyin_ime_font_ready() 就是「现在加载字体」：同步读分区/SD，
     * 期间界面冻结。先显示提示再调用。 */
    load_hint_show(mask);

    /* 字库不可用（分区/卡上没有文件、或 PSRAM 放不下）时组件会退到 LVGL 默认
     * 字体：中文候选和键名会画成方框，但 create() 照样成功。所以单独问一次。 */
    if (!lv_pinyin_ime_font_ready()) {
        ESP_LOGW(TAG, "中文字库不可用（分区或 SD 上的 ime_font_*.bin），退回英文键盘");
        load_hint_hide();
        lv_obj_delete(mask);
        ui_text_input_show(title, max_len, cb, arg);
        return;
    }

    /* 键盘挂在 mask 上：mask 是 320x240 的确定尺寸，组件的等分排布才有得算 */
    lv_obj_t *kb = lv_pinyin_ime_create(mask);
    load_hint_hide();

    if (kb == NULL || ta == NULL) {
        /* 拼音引擎没起来：词库分区缺失/内容不对，或者 PSRAM 不够。
         * 把当时的最大连续块打出来，OOM 与「词库坏了」一眼能分开 */
        ESP_LOGW(TAG, "拼音引擎不可用：PSRAM 最大块 %u KB，空闲 %u KB",
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        lv_obj_delete(mask);
        ui_text_input_show(title, max_len, cb, arg);
        return;
    }

    s_ime_mask = mask;
    s_ime_ta   = ta;
    s_ime_kb   = kb;
    s_ime_cb   = cb;
    s_ime_arg  = arg;
    lv_obj_add_event_cb(mask, ime_del_cb, LV_EVENT_DELETE, NULL);

    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_pinyin_ime_set_mode(kb, LV_PINYIN_IME_MODE_K26);
    lv_pinyin_ime_set_lang(kb, LV_PINYIN_IME_LANG_CN);
    lv_pinyin_ime_attach(kb, ta);

    /* 组件自己管按键、不走 LVGL 的 group，所以不靠 group 抢焦点。这里手动点亮焦点
     * 状态，一是与英文那一层对齐（那边 lv_keyboard_set_textarea() 顺带就点亮了），
     * 二是让输入框走主题的聚焦样式。光标闪烁不依赖这个状态：lv_textarea 的构造函数
     * 里已经启动了闪烁动画（只要主题给了光标样式的 anim_duration） */
    lv_obj_add_state(ta, LV_STATE_FOCUSED);

    /* 「确定」：背完最后一个候选之后按键上只剩确定，那时组件才发这个事件 */
    lv_obj_add_event_cb(kb, ime_ready_cb, lv_pinyin_ime_event_ready(), NULL);

    lv_obj_add_event_cb(mask, ime_show_cb, LV_EVENT_CLICKED, NULL);
}

#endif /* UI_IME_HAVE_PINYIN */