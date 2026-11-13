#include "ui_calculator.h"
#include "ui_common.h"
#include "app_navigator.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

/* ===================== 布局 =====================
 *
 *   ┌──── 标题栏（显示待运算的算式，如 "12 +"）────┐
 *   │ ┌────────────── 显示区 ──────────────┐        │
 *   │ │                          123.456 │        │
 *   │ └────────────────────────────────────┘        │
 *   │   C    ←    %    ÷                             │
 *   │   7    8    9    ×                             │
 *   │   4    5    6    -                             │
 *   │   1    2    3    +                             │
 *   │   ±    0    .    =                             │
 *   └────────────────────────────────────────────────┘
 */

#define CALC_MARGIN     8
#define CALC_DISP_Y     30
#define CALC_DISP_H     58
#define CALC_KEY_Y      90      /*!< 5 行键 + 4 道缝：横屏落在 240 内，竖屏自动变高 */
#define CALC_KEY_GAP    3
#define CALC_COLS       4
#define CALC_KEY_W      ((ui_scr_w() - CALC_MARGIN * 2 - CALC_KEY_GAP * (CALC_COLS - 1)) / CALC_COLS)
#define CALC_KEY_H      (((ui_scr_h() - CALC_KEY_Y - CALC_MARGIN) - CALC_KEY_GAP * 4) / 5)

/*! 输入串上限：10 位数字 + 可能的负号，48px 字体刚好在显示区放得下 */
#define CALC_ENTRY_MAX  11

/*! 超出这个量级就报溢出，避免显示被裁成半截 */
#define CALC_LIMIT      1e10

static lv_obj_t *s_root   = NULL;
static lv_obj_t *s_num_lb = NULL;   /*!< 结果显示 */
static lv_obj_t *s_bar    = NULL;   /*!< 标题栏：算式借它的标题位显示 */

static double s_acc     = 0.0;      /*!< 累加值（左操作数 / 结果） */
static double s_cur     = 0.0;      /*!< 当前显示值（非输入态时用） */
static char   s_entry[CALC_ENTRY_MAX + 1];  /*!< 输入串（保留小数点原文） */
static bool   s_enter   = false;    /*!< 正在输入数字 */
static char   s_pending = 0;        /*!< 待执行运算符，0 表示没有 */
static bool   s_error   = false;
static const char *s_error_msg = "错误";

/* ===================== 数字格式化 ===================== */

/**
 * @brief 把 double 写成十进制字符串
 *
 * 不用 "%.6f"：工程里不能保证 newlib 带浮点格式化支持，
 * 这里先四舍五入到 6 位小数，再拆成整数 / 小数两段手工拼。
 */
static void fmt_num(char *out, size_t n, double v)
{
    bool neg = (v < 0);
    if (neg) {
        v = -v;
    }

    long long scaled = (long long)(v * 1000000.0 + 0.5);
    long long ip     = scaled / 1000000;
    int       fp     = (int)(scaled % 1000000);

    char digits[16];
    int  dn = 0;
    if (ip == 0) {
        digits[dn++] = '0';
    }
    while (ip > 0 && dn < (int)sizeof(digits)) {
        digits[dn++] = (char)('0' + (int)(ip % 10));
        ip /= 10;
    }

    char frac[8];
    int  fn = 0;
    while (fp > 0 && fn < 6) {
        frac[fn++] = (char)('0' + fp % 10);     /* 这里先存成倒序 */
        fp /= 10;
    }

    size_t p = 0;
    if (neg && p + 1 < n) {
        out[p++] = '-';
    }
    while (dn > 0 && p + 1 < n) {
        out[p++] = digits[--dn];
    }
    if (fn > 0 && p + 2 < n) {
        out[p++] = '.';
        for (int i = fn - 1; i >= 0 && p + 1 < n; i--) {
            out[p++] = frac[i];
        }
    }
    out[p] = '\0';
}

/* ===================== 显示刷新 ===================== */

static void disp_update(void)
{
    char buf[24];

    if (s_error) {
        /* 大字体里没有汉字，错误提示切回 14px */
        lv_obj_set_style_text_font(s_num_lb, &font_ui_14, 0);
        lv_label_set_text(s_num_lb, s_error_msg);
    } else if (s_enter) {
        lv_obj_set_style_text_font(s_num_lb, &font_clock_48, 0);
        lv_label_set_text(s_num_lb, s_entry[0] != '\0' ? s_entry : "0");
    } else if (s_cur >= CALC_LIMIT || s_cur <= -CALC_LIMIT) {
        lv_obj_set_style_text_font(s_num_lb, &font_ui_14, 0);
        lv_label_set_text(s_num_lb, "数值超出范围");
    } else {
        lv_obj_set_style_text_font(s_num_lb, &font_clock_48, 0);
        fmt_num(buf, sizeof(buf), s_cur);
        lv_label_set_text(s_num_lb, buf);
    }

    /* 算式放标题栏：显示区只有 58px 高，塞不下「小算式 + 48px 数字」 */
    if (s_bar != NULL) {
        if (s_pending != 0 && !s_error) {
            char title[32];
            fmt_num(buf, sizeof(buf), s_acc);
            snprintf(title, sizeof(title), "%s %c", buf, s_pending);
            ui_titlebar_set_title(s_bar, title);
        } else {
            ui_titlebar_set_title(s_bar, "计算器");
        }
    }
}

/* ===================== 输入串 ===================== */

static double entry_value(void)
{
    double v     = 0.0;
    double scale = 0.0;
    bool   neg   = false;
    bool   dot   = false;

    for (const char *p = s_entry; *p != '\0'; p++) {
        if (*p == '-') {
            neg = true;
        } else if (*p == '.') {
            dot = true;
        } else if (*p >= '0' && *p <= '9') {
            int d = *p - '0';
            if (!dot) {
                v = v * 10.0 + d;
            } else {
                scale = (scale == 0.0) ? 0.1 : scale / 10.0;
                v += d * scale;
            }
        }
    }
    return neg ? -v : v;
}

static void calc_clear(void)
{
    s_acc     = 0.0;
    s_cur     = 0.0;
    s_entry[0] = '\0';
    s_enter   = false;
    s_pending = 0;
    s_error   = false;
    disp_update();
}

static void entry_push(char c)
{
    if (s_error) {
        calc_clear();               /* 出错后按数字：直接开新算式 */
    }

    if (!s_enter) {
        /* 新数字：丢掉上一条输入串，从空开始 */
        s_enter    = true;
        s_entry[0] = '\0';
    }

    size_t len = strlen(s_entry);

    if (c == '.') {
        if (strchr(s_entry, '.') != NULL) {
            return;
        }
        if (len == 0) {             /* 直接按小数点 → "0." */
            s_entry[len++] = '0';
        }
    }

    if (len >= CALC_ENTRY_MAX) {
        return;
    }

    s_entry[len]     = c;
    s_entry[len + 1] = '\0';
    disp_update();
}

static void entry_backspace(void)
{
    if (!s_enter) {
        return;
    }

    size_t len = strlen(s_entry);
    if (len <= 1) {
        /* 删空了就回到「没在输入」的状态，显示的是累加值 */
        s_entry[0] = '\0';
        s_enter    = false;
    } else {
        s_entry[len - 1] = '\0';
    }
    disp_update();
}

static void entry_sign(void)
{
    if (!s_enter) {
        s_enter    = true;
        s_entry[0] = '\0';
    }

    size_t len = strlen(s_entry);
    if (len > 0 && s_entry[0] == '-') {
        memmove(s_entry, s_entry + 1, len);             /* 带上结尾 '\0' */
    } else {
        if (len + 2 > sizeof(s_entry)) {
            return;
        }
        memmove(s_entry + 1, s_entry, len + 1);
        s_entry[0] = '-';
    }
    disp_update();
}

/* ===================== 运算 ===================== */

/* 运算结果：0 = 正常，1 = 除以 0，2 = 溢出 */
static int calc_apply(char op, double a, double b, double *out)
{
    switch (op) {
    case '+':
        *out = a + b;
        break;
    case '-':
        *out = a - b;
        break;
    case '*':
        *out = a * b;
        break;
    case '/':
        if (b == 0.0) {
            return 1;
        }
        *out = a / b;
        break;
    default:
        *out = b;
        break;
    }

    if (*out >= CALC_LIMIT || *out <= -CALC_LIMIT) {
        return 2;
    }
    return 0;
}

static void calc_fail(int err)
{
    s_error     = true;
    s_error_msg = (err == 1) ? "不能除以 0" : "数值超出范围";
    s_acc       = 0.0;
    s_cur       = 0.0;
    s_entry[0]  = '\0';
    s_enter     = false;
    s_pending   = 0;
    disp_update();
}

static void calc_op(char op)
{
    if (s_error) {
        return;
    }

    double v = s_enter ? entry_value() : s_cur;

    /* 连按运算符（如「5 + ×」）：只换运算符，不能拿累加值再算一遍 */
    if (!s_enter && s_pending != 0) {
        s_pending = op;
        disp_update();
        return;
    }

    if (s_pending != 0) {
        double out = 0.0;
        int    r   = calc_apply(s_pending, s_acc, v, &out);
        if (r != 0) {
            calc_fail(r);
            return;
        }
        s_acc = out;
    } else {
        s_acc = v;
    }

    s_cur      = s_acc;
    s_enter    = false;
    s_entry[0] = '\0';
    s_pending  = op;
    disp_update();
}

static void calc_equals(void)
{
    if (s_error) {
        return;
    }

    double v   = s_enter ? entry_value() : s_cur;
    double out = v;

    if (s_pending != 0) {
        int r = calc_apply(s_pending, s_acc, v, &out);
        if (r != 0) {
            calc_fail(r);
            return;
        }
    }

    s_acc      = out;
    s_cur      = out;
    s_enter    = false;
    s_entry[0] = '\0';
    s_pending  = 0;
    disp_update();
}

static void calc_percent(void)
{
    if (s_error) {
        return;
    }

    double v = s_enter ? entry_value() : s_cur;

    /* 加减法里的百分号按「左操作数的百分之几」算（200 + 10% = 220），
     * 乘除法里就是个纯比例（200 × 10% = 20），和手机计算器一致 */
    if (s_pending == '+' || s_pending == '-') {
        v = s_acc * v / 100.0;
    } else {
        v /= 100.0;
    }

    s_cur      = v;
    s_enter    = false;
    s_entry[0] = '\0';
    if (s_pending == 0) {
        s_acc = v;
    }
    disp_update();
}

/* ===================== 按键 ===================== */

static void on_key(lv_event_t *e)
{
    const char *k = (const char *)lv_event_get_user_data(e);
    if (k == NULL || s_num_lb == NULL) {
        return;
    }

    /* 出错后只认 C 和数字：C 清空，数字直接开新算式 */
    bool is_digit = (k[0] >= '0' && k[0] <= '9');
    if (s_error && strcmp(k, "C") != 0 && !is_digit && k[0] != '.') {
        return;
    }

    if (strcmp(k, "C") == 0) {
        calc_clear();
    } else if (strcmp(k, "←") == 0) {
        entry_backspace();
    } else if (strcmp(k, "%") == 0) {
        calc_percent();
    } else if (strcmp(k, "±") == 0) {
        entry_sign();
    } else if (strcmp(k, "=") == 0) {
        calc_equals();
    } else if (strcmp(k, "÷") == 0) {
        calc_op('/');
    } else if (strcmp(k, "×") == 0) {
        calc_op('*');
    } else if (strcmp(k, "-") == 0) {
        calc_op('-');
    } else if (strcmp(k, "+") == 0) {
        calc_op('+');
    } else {
        entry_push(k[0]);
    }
}

/* ===================== 页面构建 ===================== */

static void make_key(lv_obj_t *root, int row, int col, const char *text,
                     lv_color_t text_color, bool primary)
{
    lv_obj_t *btn = lv_button_create(root);
    lv_obj_set_size(btn, CALC_KEY_W, CALC_KEY_H);
    lv_obj_set_pos(btn, CALC_MARGIN + col * (CALC_KEY_W + CALC_KEY_GAP),
                   CALC_KEY_Y + row * (CALC_KEY_H + CALC_KEY_GAP));
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, primary ? UI_COLOR_PRIMARY : UI_COLOR_SURFACE, 0);
    ui_add_click_feedback(btn);
    lv_obj_add_event_cb(btn, on_key, LV_EVENT_CLICKED, (void *)text);

    lv_obj_t *lb = lv_label_create(btn);
    lv_obj_set_style_text_font(lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(lb, primary ? lv_color_white() : text_color, 0);
    lv_label_set_text(lb, text);
    lv_obj_center(lb);
}

static void calc_root_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    /* 切页是异步删除，手快「计算器 → 别的 → 计算器」时只有目标还是旧根
     * 才清空，否则清掉的是新页面的指针 */
    if (lv_event_get_target(e) != s_root) {
        return;
    }
    s_root   = NULL;
    s_num_lb = NULL;
    s_bar    = NULL;
}

void ui_calculator_create(lv_obj_t *root)
{
    s_root = root;

    /* 标题栏要留着改标题（算式），所以用 plain 版把右侧时钟位让出来 */
    s_bar = ui_titlebar_create_plain(root, "计算器", app_navigator_back_smart_cb, NULL);

    /* ===== 显示区 ===== */
    lv_obj_t *disp = lv_obj_create(root);
    lv_obj_remove_flag(disp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(disp, ui_scr_w() - CALC_MARGIN * 2, CALC_DISP_H);
    lv_obj_set_pos(disp, CALC_MARGIN, CALC_DISP_Y);
    lv_obj_set_style_bg_color(disp, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(disp, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(disp, 0, 0);
    lv_obj_set_style_radius(disp, 10, 0);
    lv_obj_set_style_pad_all(disp, 0, 0);

    s_num_lb = lv_label_create(disp);
    lv_obj_set_style_text_color(s_num_lb, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_num_lb, &font_clock_48, 0);
    /* 右对齐：位数变多时向左生长，右边固定留 8px */
    lv_obj_set_width(s_num_lb, ui_scr_w() - CALC_MARGIN * 2 - 16);
    lv_obj_set_style_text_align(s_num_lb, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_num_lb, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_num_lb, LV_ALIGN_RIGHT_MID, -8, 0);

    /* ===== 键盘 ===== */
    make_key(root, 0, 0, "C",  UI_COLOR_TEMP,    false);
    make_key(root, 0, 1, "←",  UI_COLOR_TEXT,    false);
    make_key(root, 0, 2, "%",  UI_COLOR_PRIMARY, false);
    make_key(root, 0, 3, "÷",  UI_COLOR_PRIMARY, false);

    make_key(root, 1, 0, "7",  UI_COLOR_TEXT,    false);
    make_key(root, 1, 1, "8",  UI_COLOR_TEXT,    false);
    make_key(root, 1, 2, "9",  UI_COLOR_TEXT,    false);
    make_key(root, 1, 3, "×",  UI_COLOR_PRIMARY, false);

    make_key(root, 2, 0, "4",  UI_COLOR_TEXT,    false);
    make_key(root, 2, 1, "5",  UI_COLOR_TEXT,    false);
    make_key(root, 2, 2, "6",  UI_COLOR_TEXT,    false);
    make_key(root, 2, 3, "-",  UI_COLOR_PRIMARY, false);

    make_key(root, 3, 0, "1",  UI_COLOR_TEXT,    false);
    make_key(root, 3, 1, "2",  UI_COLOR_TEXT,    false);
    make_key(root, 3, 2, "3",  UI_COLOR_TEXT,    false);
    make_key(root, 3, 3, "+",  UI_COLOR_PRIMARY, false);

    make_key(root, 4, 0, "±",  UI_COLOR_PRIMARY, false);
    make_key(root, 4, 1, "0",  UI_COLOR_TEXT,    false);
    make_key(root, 4, 2, ".",  UI_COLOR_TEXT,    false);
    make_key(root, 4, 3, "=",  UI_COLOR_PRIMARY, true);

    /* 每次进页面都从干净的 0 开始（计算器没有需要跨页保留的状态） */
    calc_clear();

    lv_obj_add_event_cb(root, calc_root_del_cb, LV_EVENT_DELETE, NULL);
}
