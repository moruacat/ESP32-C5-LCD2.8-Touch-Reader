#include "ui_oobe.h"
#include "ui_common.h"
#include "esp_log.h"

#include <math.h>
#include <string.h>

static const char *TAG = "ui_oobe";

/* ============================================================================
 * 调参区
 *
 * 时间轴的三个系数（正弦错峰、timeMs = delay*9、翻转占一半）都是照抄
 * ClassIsland OobeIntroAnimationControl 的实测行为，改一个整段节奏就散，
 * 要调就整组一起调。
 * ========================================================================== */

/*! 标题文字。原版是 11 个字符的 "ClassIsland"，这里换成同样 11 格的 "ClockReader"，
 *  节拍和收尾提前量就能按原版比例照抄。
 *  改长度不会崩（下面公式全按 OOBE_N 走），但字符变多要先调小 OOBE_CELL。 */
#define OOBE_TEXT         "ClockReader"

/*! 原版 durationMs：正弦错峰和节拍压扁的基准 */
#define OOBE_DURATION_MS  500
/*! 方块浮起后「站住」的时间，给翻牌留白 */
#define OOBE_HOLD_MS      750
/*! 字距收紧时长 */
#define OOBE_REST_MS      700
/*! 结尾整层淡出时长 */
#define OOBE_FADE_MS      220
/*! 整层入场时长（等效原版的容器缩放入场，见 ui_oobe_play 里的曲线注释） */
#define OOBE_SETTLE_MS    600

/*! 方块上浮距离：原版 50px 配 400px 高的窗口，这里 44px 配 240px */
#define OOBE_RISE_PX      44
/*! 整层入场时向上收的距离 */
#define OOBE_SETTLE_PX    16

/*! 方块边长 = 字格宽度。11 格 + 10 个间距要塞进 320：
 *  11*22 + 10*3 = 272，左右各留 24px。再大就顶边了。 */
#define OOBE_CELL         22
/*! 初始间距：22 配 3 ≈ 0.136，和原版 32 配 4 的 0.125 接近 */
#define OOBE_GAP          3
/*! 翻转铰链的垂直位置：原版 12/32 = 37.5%，22*0.375 ≈ 8 */
#define OOBE_PIVOT_Y      8
/*! 方块圆角 */
#define OOBE_RADIUS       6

#define OOBE_PI           3.14159265f
/*! lv_anim_set_bezier3_param() 的定点刻度（与 LV_BEZIER_VAL_MAX 一致） */
#define OOBE_BEZIER_UNIT  1024.0f

/*! 字标字体：24px Montserrat。
 *  没选 font_ui_14 是因为 14px 在 240 高的屏上太小；font_clock_48 只收了
 *  0x20-0x3A（数字和标点），没有字母。sdkconfig 里 CONFIG_LV_FONT_MONTSERRAT_24=y。 */
extern const lv_font_t lv_font_montserrat_24;
#define OOBE_FONT         (&lv_font_montserrat_24)

/*! 字符数（编译期从字符串长度得出，不含结尾 0） */
#define OOBE_N            ((int)(sizeof(OOBE_TEXT) - 1))

_Static_assert(OOBE_N >= 4, "OOBE_TEXT 至少 4 个字符（收尾提前量取的是倒数第 3 个）");

/* ============================================================================
 * 状态
 * ========================================================================== */

/*! 一个字符的两副面孔：正面是方块，背面是字母 */
typedef struct {
    lv_obj_t  *block;    /*!< 正面：圆角方块 */
    lv_obj_t  *text;     /*!< 背面：单个字母 */
    lv_coord_t x_grid;   /*!< 收紧前的网格 x */
    lv_coord_t x_nat;    /*!< 收紧后的自然排版 x */
    lv_coord_t w_nat;    /*!< 字母自然宽度（收紧终点） */
} oobe_item_t;

/*! 单字的三个时间点 */
typedef struct {
    uint32_t rise_at;    /*!< 开始上浮 */
    uint32_t rise_ms;    /*!< 上浮时长（对应原版的 timeMs） */
    uint32_t flip_at;    /*!< 方块开始翻走（0→90°） */
    uint32_t flip_ms;    /*!< 单段翻转时长；整次翻牌由方块、字母两段首尾相接组成 */
    uint32_t text_at;    /*!< 字母开始翻入（−90°→0°）= flip_at + flip_ms */
} oobe_timing_t;

static lv_obj_t   *s_layer;              /*!< 全屏覆盖层，播完自己删 */
static oobe_item_t s_items[OOBE_N];
static oobe_timing_t s_tm[OOBE_N];
static uint32_t    s_rest_at;            /*!< 字距收紧的起点 */
static bool        s_playing;

/* ============================================================================
 * 小工具
 * ========================================================================== */

/*! CSS 的 cubic-bezier 控制点 → LVGL 的定点参数 */
static void oobe_bezier(lv_anim_t *a, float x1, float y1, float x2, float y2)
{
    lv_anim_set_bezier3_param(a,
                              (int16_t)(x1 * OOBE_BEZIER_UNIT),
                              (int16_t)(y1 * OOBE_BEZIER_UNIT),
                              (int16_t)(x2 * OOBE_BEZIER_UNIT),
                              (int16_t)(y2 * OOBE_BEZIER_UNIT));
}

/*! 千分比 0..1000 → 不透明度 0..255 */
static lv_opa_t oobe_opa(int32_t v)
{
    return (lv_opa_t)((v * LV_OPA_COVER) / 1000);
}

/*! 铰链贴在左边框：翻牌时左边不动、右边向里收，像一扇往左开的门。
 *  原版是 RenderTransformOrigin="0, 12"（32 高的 37.5%），这里照抄比例。
 *  注：LVGL 没有 3D 透视，纯 scale_x 压缩看上去是「压扁」而不是「近大远小」，
 *  但配上 90° 处换内容的时机，肉眼仍然读得出是同一张卡片翻过去。 */
static void oobe_flip_pivot(lv_obj_t *o)
{
    lv_obj_set_style_transform_pivot_x(o, 0, 0);          /* 0 = 像素 0，即左边框 */
    lv_obj_set_style_transform_pivot_y(o, OOBE_PIVOT_Y, 0);
}

/*! 单个字符的排版宽度（用来算收紧终点） */
static lv_coord_t oobe_text_width(const char *s)
{
    lv_point_t sz = { 0, 0 };
    lv_text_get_size(&sz, s, OOBE_FONT, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return (lv_coord_t)sz.x;
}

/* ============================================================================
 * 动画执行回调
 *
 * 统一用 0..1000 的千分比做量纲：同一套缓动就能喂给位置、缩放、透明度，
 * 也省得给每种属性写一个几乎一样的函数。
 * ========================================================================== */

/*! 方块上浮：translate_y 由下往上归零，同时淡入 */
static void exec_rise(void *var, int32_t v)
{
    lv_obj_t *o = (lv_obj_t *)var;
    lv_obj_set_style_translate_y(o, (lv_coord_t)((OOBE_RISE_PX * (1000 - v)) / 1000), 0);
    lv_obj_set_style_opa(o, oobe_opa(v), 0);
}

/*! 方块翻走：绕左边缘 scale_x 1→0，透明度同步归零（侧对观众时正好消失） */
static void exec_block_flip(void *var, int32_t v)
{
    lv_obj_t *o = (lv_obj_t *)var;
    lv_obj_set_style_transform_scale_x(o, (LV_SCALE_NONE * (1000 - v)) / 1000, 0);
    lv_obj_set_style_opa(o, oobe_opa(1000 - v), 0);
}

/*! 字母翻入：同一时刻 scale_x 0→1，透明度从 0 拉满 */
static void exec_text_flip(void *var, int32_t v)
{
    lv_obj_t *o = (lv_obj_t *)var;
    lv_obj_set_style_transform_scale_x(o, (LV_SCALE_NONE * v) / 1000, 0);
    lv_obj_set_style_opa(o, oobe_opa(v), 0);
}

/*! 字距收紧：格子的 x 与宽度一起动，字符靠 text_align=center 自己保持居中 */
static void exec_rest(void *var, int32_t v)
{
    oobe_item_t *it = (oobe_item_t *)var;
    if (it->text == NULL) {
        return;
    }
    lv_obj_set_x(it->text, it->x_grid + ((it->x_nat - it->x_grid) * v) / 1000);
    lv_obj_set_width(it->text, OOBE_CELL + ((it->w_nat - OOBE_CELL) * v) / 1000);
}

/*! 整层入场：整体向上收 */
static void exec_settle(void *var, int32_t v)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, (lv_coord_t)((OOBE_SETTLE_PX * (1000 - v)) / 1000), 0);
}

/*! 整层淡出（连黑底一起，下面已经建好的页面就露出来了） */
static void exec_fade(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, oobe_opa(1000 - v), 0);
}

/* ============================================================================
 * 时间轴
 * ========================================================================== */

/**
 * @brief 生成整段动画的时间表
 *
 * 原版的两级公式：
 *
 *   delay[i] = sin((i+2)/(n+2) * π/2) * durationMs / n
 *   timeMs[i] = delay[i] * 9
 *
 * 第一级决定方块的起步间隔：正弦从 ~0.24 rad 走到 ~1.45 rad，delay 由小到大、
 * 增量由大到小 —— 所以方块是「先松后紧」地起跑。
 *
 * 第二级是整段动画高级感的关键：翻转起点 = 累计 delay + timeMs + 750，而 timeMs
 * 把 delay 放大了 9 倍，两者相抵之后翻转起点的间隔变成 58/61/64/65/65/64/63/60/57/53ms
 * 这样一个接近等距的节拍 —— 入场是散的，翻牌是齐的。
 */
static void oobe_timing_build(void)
{
    float acc = 0.0f;

    for (int i = 0; i < OOBE_N; i++) {
        const float d = sinf(((float)(i + 2) / (float)(OOBE_N + 2)) * (OOBE_PI / 2.0f))
                        * (float)OOBE_DURATION_MS / (float)OOBE_N;

        s_tm[i].rise_at = (uint32_t)(acc + 0.5f);
        s_tm[i].rise_ms = (uint32_t)(d * 9.0f + 0.5f);
        s_tm[i].flip_ms = s_tm[i].rise_ms / 2;
        s_tm[i].flip_at = s_tm[i].rise_at + s_tm[i].rise_ms + OOBE_HOLD_MS;
        s_tm[i].text_at = s_tm[i].flip_at + s_tm[i].flip_ms;

        acc += d;
    }

    /* 收尾提前：原版等的是倒数第 3 个字符（原版 "ClassIsland" 里是 "a"）落定，
     * 而不是等最后一个字母。提前约 130ms 让收紧压住最后两个字母的落定，
     * 避免结尾出现一拍空等。这是原版的既有行为，不是笔误。 */
    s_rest_at = s_tm[OOBE_N - 3].text_at + s_tm[OOBE_N - 3].flip_ms;
}

/* ============================================================================
 * 生命周期
 * ========================================================================== */

/*! 删掉本模块创建的所有动画（var 传 NULL = 不限定对象，只按 exec 回调区分） */
static void oobe_stop_anims(void)
{
    lv_anim_delete(NULL, exec_rise);
    lv_anim_delete(NULL, exec_block_flip);
    lv_anim_delete(NULL, exec_text_flip);
    lv_anim_delete(NULL, exec_rest);
    lv_anim_delete(NULL, exec_settle);
    lv_anim_delete(NULL, exec_fade);
}

/*! 立刻清场（重播或页面销毁时用）。先删动画再删对象，避免回调里碰野指针 */
static void oobe_destroy(void)
{
    oobe_stop_anims();

    if (s_layer != NULL && lv_obj_is_valid(s_layer)) {
        lv_obj_delete(s_layer);
    }
    s_layer = NULL;
    s_playing = false;
    memset(s_items, 0, sizeof(s_items));
}

/*! 淡出走完：异步删掉覆盖层。
 *  不能在动画回调里同步删 —— 这一帧动画链表还在遍历，覆盖层上挂着的
 *  事件/定时器也未必都走完了。 */
static void on_fade_done(lv_anim_t *a)
{
    (void)a;
    s_playing = false;
    if (s_layer != NULL && lv_obj_is_valid(s_layer)) {
        lv_obj_delete_async(s_layer);
    }
    s_layer = NULL;
}

/*! 建一条动画（统一挂 cubic-bezier 曲线） */
typedef enum {
    OOBE_CURVE_IN = 0,   /*!< 原版方块翻走：(0.32, 0, 0.67, 0) */
    OOBE_CURVE_OUT,      /*!< 原版字母翻入：(0.33, 1, 0.68, 1) */
    OOBE_CURVE_IN_OUT,   /*!< 字距收紧：(0.65, 0, 0.35, 1) */
    OOBE_CURVE_ENTRY,    /*!< 原版容器入场：(0, 0.83, 0.17, 0.9) */
} oobe_curve_t;

static void oobe_anim_start(void *var, lv_anim_exec_xcb_t cb,
                            uint32_t delay, uint32_t dur, oobe_curve_t curve)
{
    if (dur == 0) {
        dur = 1;                      /* 时长 0 在 LVGL 里会立刻跳到终值，没有意义 */
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_duration(&a, dur);
    lv_anim_set_path_cb(&a, lv_anim_path_custom_bezier3);

    switch (curve) {
    case OOBE_CURVE_IN:
        oobe_bezier(&a, 0.32f, 0.0f, 0.67f, 0.0f);
        break;
    case OOBE_CURVE_OUT:
        oobe_bezier(&a, 0.33f, 1.0f, 0.68f, 1.0f);
        break;
    case OOBE_CURVE_ENTRY:
        oobe_bezier(&a, 0.0f, 0.83f, 0.17f, 0.9f);
        break;
    default:
        oobe_bezier(&a, 0.65f, 0.0f, 0.35f, 1.0f);
        break;
    }

    lv_anim_start(&a);
}

/* ============================================================================
 * 对外接口
 * ========================================================================== */

bool ui_oobe_is_playing(void)
{
    return s_playing;
}

void ui_oobe_play(void)
{
    if (lv_display_get_default() == NULL) {
        return;
    }

    oobe_destroy();                   /* 重播：先把上一轮（可能还在播）清干净 */

    const lv_coord_t sw = ui_scr_w();
    const lv_coord_t sh = ui_scr_h();
    if (sw <= 0 || sh <= 0) {
        return;
    }

    /* 格子总宽居中；方块行对齐屏幕中心；文字行因为行高比方块高，按中心对齐 */
    const lv_coord_t grid_w = (lv_coord_t)(OOBE_N * OOBE_CELL + (OOBE_N - 1) * OOBE_GAP);
    const lv_coord_t grid_x = (sw - grid_w) / 2;
    const lv_coord_t row_y  = (sh - OOBE_CELL) / 2;
    const lv_coord_t font_h = (lv_coord_t)lv_font_get_line_height(OOBE_FONT);
    const lv_coord_t text_y = row_y + (OOBE_CELL - font_h) / 2;

    oobe_timing_build();

    /* ---- 覆盖层：全屏黑底，吃掉触摸，播完自己消失 ---- */
    s_layer = lv_obj_create(lv_screen_active());
    lv_obj_remove_flag(s_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_layer, LV_OBJ_FLAG_CLICKABLE);   /* 挡住下面页面的滑动手势 */
    lv_obj_set_size(s_layer, sw, sh);
    lv_obj_set_pos(s_layer, 0, 0);
    lv_obj_set_style_bg_color(s_layer, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_layer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_layer, 0, 0);
    lv_obj_set_style_radius(s_layer, 0, 0);
    lv_obj_set_style_pad_all(s_layer, 0, 0);
    lv_obj_set_style_translate_y(s_layer, OOBE_SETTLE_PX, 0);

    /* ---- 两套完全重合的格子：方块一套、字母一套 ---- */
    int32_t nat_total = 0;
    for (int i = 0; i < OOBE_N; i++) {
        const char ch[2] = { OOBE_TEXT[i], '\0' };
        const lv_coord_t x = grid_x + (lv_coord_t)(i * (OOBE_CELL + OOBE_GAP));

        lv_obj_t *block = lv_obj_create(s_layer);
        lv_obj_remove_flag(block, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(block, OOBE_CELL, OOBE_CELL);
        lv_obj_set_pos(block, x, row_y);
        lv_obj_set_style_bg_color(block, UI_COLOR_TEXT, 0);
        lv_obj_set_style_bg_opa(block, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(block, OOBE_RADIUS, 0);
        lv_obj_set_style_border_width(block, 0, 0);
        lv_obj_set_style_pad_all(block, 0, 0);
        lv_obj_set_style_opa(block, LV_OPA_TRANSP, 0);          /* 起始隐藏交给动画兜 */
        lv_obj_set_style_translate_y(block, OOBE_RISE_PX, 0);
        oobe_flip_pivot(block);

        lv_obj_t *text = lv_label_create(s_layer);
        lv_label_set_text(text, ch);
        lv_obj_set_style_text_font(text, OOBE_FONT, 0);
        lv_obj_set_style_text_color(text, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_all(text, 0, 0);
        lv_obj_set_width(text, OOBE_CELL);                      /* 字格强制等宽，这是对齐的前提 */
        lv_obj_set_pos(text, x, text_y);
        lv_obj_set_style_opa(text, LV_OPA_TRANSP, 0);
        lv_obj_set_style_transform_scale_x(text, 0, 0);         /* 字母一开始侧对屏幕 */
        oobe_flip_pivot(text);

        s_items[i].block  = block;
        s_items[i].text   = text;
        s_items[i].x_grid = x;
        s_items[i].w_nat  = oobe_text_width(ch);
        nat_total += s_items[i].w_nat;
    }

    /* ---- 收紧终点：按真实字宽重新排一遍并居中 ---- */
    lv_coord_t xn = (lv_coord_t)((sw - nat_total) / 2);
    for (int i = 0; i < OOBE_N; i++) {
        s_items[i].x_nat = xn;
        xn += s_items[i].w_nat;
    }

    /* ---- ① 整层入场 ----
     * 原版是 Scale 1.25→1.0 + Opacity 0→1，曲线 cubic-bezier(0, 0.83, 0.17, 0.9)。
     * 对整屏做 scale 在 LVGL 里要给 320x240 建一层变换画布（每帧全屏重采样），
     * 单核 C5 上不划算，这里换成「整体向上收 16px」—— 这条曲线 17% 处就完成
     * 90%，本来就是一条很急促的入场，位移和缩放的前半段观感几乎等价；而
     * 「从无到有」由方块自己的淡入承担。 */
    oobe_anim_start(s_layer, exec_settle, 0, OOBE_SETTLE_MS, OOBE_CURVE_ENTRY);

    s_playing = true;

    /* ---- ②③④⑤ 每个字符 ----
     * 一次翻牌由两段首尾相接组成，不是同时：
     *
     *   方块 0→90°（ease-in）  ┃  字母 −90°→0°（ease-out）
     *   ┗━━━━━ flip_ms ━━━━━┛  ┗━━━━━ flip_ms ━━━━━┛
     *                          ↑ 两段在这里交接
     *
     * 方块转到 90°（侧对屏幕、宽度压成 0）的同一时刻，字母从 −90° 开始回正，
     * 所以看到的是「同一张卡片继续转过去露出背面」。缓动一进一出互补，
     * 接缝处速度是连续的；透明度又都在侧对时归零，藏住了内容交换的那一帧。 */
    for (int i = 0; i < OOBE_N; i++) {
        const oobe_timing_t *t = &s_tm[i];

        /* ② 方块上浮 + 淡入 */
        oobe_anim_start(s_items[i].block, exec_rise,
                        t->rise_at, t->rise_ms, OOBE_CURVE_OUT);

        /* ③ 方块翻走（0→90°） */
        oobe_anim_start(s_items[i].block, exec_block_flip,
                        t->flip_at, t->flip_ms, OOBE_CURVE_IN);

        /* ④ 字母翻入（−90°→0°），接在方块翻完之后 */
        oobe_anim_start(s_items[i].text, exec_text_flip,
                        t->text_at, t->flip_ms, OOBE_CURVE_OUT);

        /* ⑤ 字距收紧 */
        oobe_anim_start(&s_items[i], exec_rest,
                        s_rest_at, OOBE_REST_MS, OOBE_CURVE_IN_OUT);
    }

    /* ---- ⑥ 整层淡出，露出下面已经建好的页面 ---- */
    {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_layer);
        lv_anim_set_exec_cb(&a, exec_fade);
        lv_anim_set_values(&a, 0, 1000);
        lv_anim_set_delay(&a, s_rest_at + OOBE_REST_MS);
        lv_anim_set_duration(&a, OOBE_FADE_MS);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_completed_cb(&a, on_fade_done);
        lv_anim_start(&a);
    }

    ESP_LOGI(TAG, "%d 格字标：总时长 %ums（收紧 %ums 起 %ums，淡出 %ums）",
             OOBE_N,
             (unsigned)(s_rest_at + OOBE_REST_MS + OOBE_FADE_MS),
             (unsigned)s_rest_at, (unsigned)OOBE_REST_MS, (unsigned)OOBE_FADE_MS);
}
