#include "ui_oobe.h"
#include "ui_common.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

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
/*! 结尾整体淡出时长 */
#define OOBE_FADE_MS      220

/*! 方块上浮距离：原版 50px 配 400px 高的窗口，这里 44px 配 240px */
#define OOBE_RISE_PX      44

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

/*! 置 0 可以整段跳过开机动画。排查「是不是它拖慢了整机」时最省事的一刀：
 *  改这一个数字重编译，其余代码不用动。 */
#define OOBE_ENABLE       1

_Static_assert(OOBE_N >= 4, "OOBE_TEXT 至少 4 个字符（收尾提前量取的是倒数第 3 个）");

/* ============================================================================
 * 状态
 * ========================================================================== */

/*! 一个字符的两副面孔：正面是方块，背面是字母 */
typedef struct {
    lv_obj_t  *block;    /*!< 正面：圆角方块 */
    lv_obj_t  *text;     /*!< 背面：单个字母 */
    lv_coord_t x_start;  /*!< 收紧前的渲染 x（字符在 22px 格子里的居中位置） */
    lv_coord_t x_nat;    /*!< 收紧后的自然排版 x */
    lv_coord_t w_nat;    /*!< 字母自然宽度（创建时就固定成这个宽度，全程不改） */
} oobe_item_t;

/*! 单字的时间点 */
typedef struct {
    uint32_t rise_at;    /*!< 开始上浮 */
    uint32_t rise_ms;    /*!< 上浮时长（对应原版的 timeMs） */
    uint32_t flip_at;    /*!< 方块开始翻走（0→90°） */
    uint32_t flip_ms;    /*!< 单段翻转时长；整次翻牌由方块、字母两段首尾相接组成 */
    uint32_t text_at;    /*!< 字母开始翻入（−90°→0°）= flip_at + flip_ms */
} oobe_timing_t;

/* 动画跑在**独立 screen** 上，而不是叠在当前页面之上的一个覆盖层。
 * 叠加层看着省事，但 LVGL 不做遮挡剔除：每次局部重绘都要把
 * 「screen 黑底 + 页面黑底 + 覆盖层黑底」画三遍，320x240 的屏直接掉帧；
 * 独立 screen 的对象树里只有这 22 个元素，重绘成本只剩一份。 */
static lv_obj_t   *s_screen;         /*!< OOBE 自己的那一屏 */
static lv_obj_t   *s_prev_screen;    /*!< 播完要切回去的页面 */
static lv_timer_t *s_watchdog;       /*!< 兜底：动画回调没跑到也要收尾 */
static lv_timer_t *s_probe;          /*!< 帧率探针：数动画期间 LVGL 处理了多少个刷新周期 */
static uint32_t    s_probe_frames;
static uint32_t    s_expect_ms;      /*!< 本轮动画的标称总时长，用来算满帧数 */
static oobe_item_t s_items[OOBE_N];
static oobe_timing_t s_tm[OOBE_N];
static uint32_t    s_rest_at;        /*!< 字距收紧的起点 */
static uint32_t    s_start_tick;     /*!< 开始时的时间戳，只用于统计实际耗时 */
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

/*! 下面两个计数函数定义在生命周期那一节，这里先声明 */
static uint32_t oobe_count_timers(void);
static uint32_t oobe_count_objs(const lv_obj_t *o);

/*! 收尾后复查一次：隔 3 秒再数一遍。两次数据一样说明干净，
 *  对象数/定时器数还在涨就是泄漏 —— 那才是「整机一直卡」的典型症状。 */
static void on_diag_late(lv_timer_t *t)
{
    lv_timer_delete(t);
    ESP_LOGI(TAG, "3 秒后：动画=%u 定时器=%u 活动屏对象=%u 内部RAM=%u PSRAM=%u",
             (unsigned)lv_anim_count_running(),
             (unsigned)oobe_count_timers(),
             (unsigned)oobe_count_objs(lv_screen_active()),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
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

/*! 字距收紧：只动 x。
 *
 *  这里是整段动画最大的性能雷区：给 label 改宽度会触发
 *  lv_obj_refresh_self_size() —— 只要宽或高有一个是 LV_SIZE_CONTENT 它就
 *  lv_obj_mark_layout_as_dirty()，把 **整个屏幕**标记成布局脏（scr_layout_inv），
 *  下一帧 layout_update_core() 递归遍历整屏对象树、把每个脏对象的尺寸和位置
 *  重算一遍，label 还会顺便把文本宽度重新测量一次。11 个字 × 42 帧 = 462 次
 *  全屏重排，帧率必崩。
 *
 *  所以 label 的宽高在创建时就固定成「自然宽度 × 行高」，全程不变；
 *  收紧只移动 x —— 绝对定位不参与布局，只会 invalidate 那两个小矩形。 */
static void exec_rest(void *var, int32_t v)
{
    oobe_item_t *it = (oobe_item_t *)var;
    if (it->text == NULL) {
        return;
    }
    lv_obj_set_x(it->text, it->x_start + ((it->x_nat - it->x_start) * v) / 1000);
}

/*! 单个字符淡出。
 *
 *  这里特意**不**对整屏做 opa 动画：屏幕 320x240，LCD 走 40MHz SPI，绘制缓冲
 *  只有 4800 像素（约 15 行）高，整屏刷新要分 12 次 flush ≈ 30ms —— 一帧就吃掉
 *  两个 LVGL 刷新周期，220ms 的整屏淡出只能画出六七帧，肉眼就是一顿。
 *  只淡出 11 个字母时脏区是 272x29，两次 flush 就够，成本差一个数量级。
 *
 *  背景本来就是纯黑，时钟页也是纯黑，所以最后直接切屏不会有颜色跳变。 */
static void exec_fade_item(void *var, int32_t v)
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
    lv_anim_delete(NULL, exec_fade_item);
}

/*! 数一下当前 LVGL 里的定时器个数（诊断用，见 oobe_teardown） */
static uint32_t oobe_count_timers(void)
{
    uint32_t n = 0;
    for (lv_timer_t *t = lv_timer_get_next(NULL); t != NULL; t = lv_timer_get_next(t)) {
        n++;
    }
    return n;
}

/*! 递归数对象树（诊断用） */
static uint32_t oobe_count_objs(const lv_obj_t *o)
{
    if (o == NULL) {
        return 0;
    }
    uint32_t n = 1;
    const uint32_t cnt = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < cnt; i++) {
        n += oobe_count_objs(lv_obj_get_child(o, (int32_t)i));
    }
    return n;
}

/**
 * @brief 收尾：清动画、清定时器、切回原页面、删掉 OOBE 屏
 *
 * 顺序很重要：先把屏幕切回去，再删 OOBE 屏。否则当前活动屏被删掉时，
 * LVGL 会落到一个没有 screen 的状态，表现为整屏黑掉或者花屏。
 */
static void oobe_teardown(void)
{
    oobe_stop_anims();

    if (s_watchdog != NULL) {
        lv_timer_delete(s_watchdog);
        s_watchdog = NULL;
    }

    if (s_probe != NULL) {
        lv_timer_delete(s_probe);
        s_probe = NULL;
        const uint32_t full = s_expect_ms / 15u;      /* LV_DEF_REFR_PERIOD */
        ESP_LOGI(TAG, "刷新周期 %u / 满帧 %u（约 %u%%）",
                 (unsigned)s_probe_frames, (unsigned)full,
                 (unsigned)(full ? (s_probe_frames * 100u) / full : 0u));
    }

    lv_obj_t *scr  = s_screen;
    lv_obj_t *prev = s_prev_screen;
    s_screen      = NULL;
    s_prev_screen = NULL;
    s_playing     = false;

    if (prev != NULL && lv_obj_is_valid(prev)) {
        lv_screen_load(prev);
    }
    if (scr != NULL && lv_obj_is_valid(scr)) {
        lv_obj_delete_async(scr);
    }

    memset(s_items, 0, sizeof(s_items));

    /* 收尾体检：这三个数都该回到「动画开始前」的水平。
     * 动画残留 / 定时器没删 / 堆被吃光，都会在这里露出来 ——
     * 「整机一直卡」基本都出在这一步没做干净。 */
    ESP_LOGI(TAG, "收尾：动画=%u 定时器=%u 活动屏对象=%u 内部RAM=%u PSRAM=%u",
             (unsigned)lv_anim_count_running(),
             (unsigned)oobe_count_timers(),
             (unsigned)oobe_count_objs(lv_screen_active()),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    lv_timer_t *late = lv_timer_create(on_diag_late, 3000, NULL);
    if (late != NULL) {
        lv_timer_set_repeat_count(late, 1);
    }
}

/*! 淡出走完。
 *  不能在动画回调里同步删 —— 这一帧动画链表还在遍历，屏幕上挂着的
 *  定时器/事件也未必都走完了。 */
static void on_fade_done(lv_anim_t *a)
{
    (void)a;
    ESP_LOGI(TAG, "动画结束，实际 %ums", (unsigned)lv_tick_elaps(s_start_tick));
    oobe_teardown();
}

/*! 帧率探针：每被 LVGL 调度一次就 +1。
 *  它的周期就是 LVGL 的刷新周期（LV_DEF_REFR_PERIOD = 15ms），所以
 *  「实际触发次数 / 标称总时长 ÷ 15」就是这段时间的帧率占比。
 *  动画是按 tick 时间推进的，卡顿只会丢帧不会拉长总时长，光看总耗时看不出问题。 */
static void on_probe(lv_timer_t *t)
{
    (void)t;
    s_probe_frames++;
}

/*! 兜底：万一淡出动画因为卡顿丢了帧、或者别的原因没跑到 completed_cb，
 *  到点也强制收尾，绝不让这一屏留在屏幕上拖累刷新率。 */
static void on_watchdog(lv_timer_t *t)
{
    s_watchdog = NULL;
    lv_timer_delete(t);               /* 一次性定时器，自己删掉 */
    if (s_screen != NULL) {
        ESP_LOGW(TAG, "动画超时未收尾，强制清理");
        oobe_teardown();
    }
}

/*! 建一条动画（统一挂 cubic-bezier 曲线） */
typedef enum {
    OOBE_CURVE_IN = 0,   /*!< 原版方块翻走：(0.32, 0, 0.67, 0) */
    OOBE_CURVE_OUT,      /*!< 原版字母翻入：(0.33, 1, 0.68, 1) */
    OOBE_CURVE_IN_OUT,   /*!< 字距收紧：(0.65, 0, 0.35, 1) */
} oobe_curve_t;

/*! 方块翻到 90° 之后就没用了：复位 scale 并隐藏。
 *  不复位的话它会一直以 LV_LAYER_TYPE_TRANSFORM 的身份留在对象树里，
 *  后续每一帧都要为它算绘制区域（虽然 opa=0 不会真画），没必要。 */
static void on_block_flipped(lv_anim_t *a)
{
    lv_obj_t *o = (lv_obj_t *)a->var;
    if (o == NULL) {
        return;
    }
    lv_obj_set_style_transform_scale_x(o, LV_SCALE_NONE, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void oobe_anim_start(void *var, lv_anim_exec_xcb_t cb,
                            uint32_t delay, uint32_t dur, oobe_curve_t curve,
                            lv_anim_completed_cb_t done)
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
    default:
        oobe_bezier(&a, 0.65f, 0.0f, 0.35f, 1.0f);
        break;
    }

    if (done != NULL) {
        lv_anim_set_completed_cb(&a, done);
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
    if (!OOBE_ENABLE) {
        return;
    }
    if (lv_display_get_default() == NULL) {
        return;
    }

    oobe_teardown();                  /* 重播：先把上一轮（可能还在播）清干净 */
    memset(s_tm, 0, sizeof(s_tm));

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

    /* ---- OOBE 自己的一屏：黑底、吃掉触摸，播完整屏扔掉 ---- */
    s_prev_screen = lv_screen_active();
    s_screen = lv_obj_create(NULL);
    if (s_screen == NULL) {
        ESP_LOGE(TAG, "创建 OOBE 屏幕失败（内存不足？），跳过动画");
        s_prev_screen = NULL;
        return;
    }
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_screen, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_radius(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);

    /* ---- 两套完全重合的格子：方块一套、字母一套 ---- */
    int32_t nat_total = 0;
    for (int i = 0; i < OOBE_N; i++) {
        const char ch[2] = { OOBE_TEXT[i], '\0' };
        const lv_coord_t x = grid_x + (lv_coord_t)(i * (OOBE_CELL + OOBE_GAP));

        lv_obj_t *block = lv_obj_create(s_screen);
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

        /* 字符在 22px 格子里居中 —— 这个位置就是收紧动画的起点，
         * 宽度固定成自然宽度后它和原来的「MinWidth=32 + 居中」是等效的。 */
        const lv_coord_t w_nat   = oobe_text_width(ch);
        const lv_coord_t x_start = x + (OOBE_CELL - w_nat) / 2;

        lv_obj_t *text = lv_label_create(s_screen);
        lv_label_set_text(text, ch);
        lv_obj_set_style_text_font(text, OOBE_FONT, 0);
        lv_obj_set_style_text_color(text, UI_COLOR_TEXT, 0);
        lv_obj_set_style_pad_all(text, 0, 0);
        /* 宽高一次性固定死。只要有一个维度留在 LV_SIZE_CONTENT，
         * 后面的 lv_obj_set_width() 就会走 lv_obj_mark_layout_as_dirty()，
         * 把整屏标记成布局脏 —— 那是每帧一次全屏重排，帧率直接崩。 */
        lv_obj_set_size(text, w_nat, font_h);
        lv_obj_set_pos(text, x_start, text_y);
        lv_obj_set_style_opa(text, LV_OPA_TRANSP, 0);
        lv_obj_set_style_transform_scale_x(text, 0, 0);         /* 字母一开始侧对屏幕 */
        oobe_flip_pivot(text);

        s_items[i].block   = block;
        s_items[i].text    = text;
        s_items[i].x_start = x_start;
        s_items[i].w_nat   = w_nat;
        nat_total += w_nat;
    }

    /* ---- 收紧终点：按真实字宽重新排一遍并居中 ---- */
    lv_coord_t xn = (lv_coord_t)((sw - nat_total) / 2);
    for (int i = 0; i < OOBE_N; i++) {
        s_items[i].x_nat = xn;
        xn += s_items[i].w_nat;
    }

    /* 切到 OOBE 这一屏。此刻 22 个元素都还是「方块 opa=0 / 字母 scale=0」，
     * 所以观众看到的是纯黑，不会闪出半成品。 */
    lv_screen_load(s_screen);

    s_start_tick = lv_tick_get();
    s_playing = true;

    /* ---- 每个字符：上浮 → 方块翻走 → 字母翻入 → 字距收紧 ----
     *
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

        /* 方块上浮 + 淡入 */
        oobe_anim_start(s_items[i].block, exec_rise,
                        t->rise_at, t->rise_ms, OOBE_CURVE_OUT, NULL);

        /* 方块翻走（0→90°），翻完就把它藏掉 */
        oobe_anim_start(s_items[i].block, exec_block_flip,
                        t->flip_at, t->flip_ms, OOBE_CURVE_IN, on_block_flipped);

        /* 字母翻入（−90°→0°），接在方块翻完之后 */
        oobe_anim_start(s_items[i].text, exec_text_flip,
                        t->text_at, t->flip_ms, OOBE_CURVE_OUT, NULL);

        /* 字距收紧 */
        oobe_anim_start(&s_items[i], exec_rest,
                        s_rest_at, OOBE_REST_MS, OOBE_CURVE_IN_OUT, NULL);
    }

    /* ---- 收尾：只淡出字母，最后一条走完就切回原页面 ---- */
    const uint32_t total_ms = s_rest_at + OOBE_REST_MS + OOBE_FADE_MS;
    const uint32_t fade_at  = s_rest_at + OOBE_REST_MS;
    for (int i = 0; i < OOBE_N; i++) {
        oobe_anim_start(s_items[i].text, exec_fade_item, fade_at, OOBE_FADE_MS,
                        OOBE_CURVE_IN_OUT, (i == 0) ? on_fade_done : NULL);
    }

    /* 兜底定时器：给整段留 1.5 秒余量。卡顿严重的机器上宁可硬收，也不留残影 */
    s_watchdog = lv_timer_create(on_watchdog, total_ms + 1500, NULL);
    if (s_watchdog != NULL) {
        lv_timer_set_repeat_count(s_watchdog, 1);
    }

    /* 帧率探针（只在日志里用，成本是一年几十次加法） */
    s_expect_ms = total_ms;
    s_probe_frames = 0;
    s_probe = lv_timer_create(on_probe, 15, NULL);

    ESP_LOGI(TAG, "%d 格字标：总时长 %ums（收紧 %ums 起 %ums，淡出 %ums）",
             OOBE_N, (unsigned)total_ms,
             (unsigned)s_rest_at, (unsigned)OOBE_REST_MS, (unsigned)OOBE_FADE_MS);
}
