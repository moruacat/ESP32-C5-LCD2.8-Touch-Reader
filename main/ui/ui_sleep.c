#include "ui_sleep.h"
#include "ui_common.h"
#include "app_navigator.h"

#include "srv_alarm.h"
#include "srv_config.h"
#include "srv_input.h"
#include "srv_player.h"
#include "srv_radio.h"
#include "srv_recorder.h"
#include "ui_timer.h"
#include "ui_countdown.h"

#include "bsp/esp32_c5_touch_lcd_2.8.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "ui_sleep";

/* 变暗后取用户亮度的百分之多少（相对亮度，不是固定值） */
#define DIM_RATIO_PCT   20
/* 判定周期。真正的「有操作」靠触摸事件即时续期（见 on_indev_activity），
 * 这个定时器只负责判断「闲置够久了」以及阅读器免打扰这类状态。 */
#define DIM_TICK_MS     300

static lv_obj_t *s_layer = NULL;    /*!< 盖在最上层的黑屏，同时吃掉所有触摸 */
static bool      s_on    = false;

static int64_t   s_last_act_us = 0;
static bool      s_dimmed      = false;
static bool      s_locked      = false;     /*!< 本次闲置是否已经锁过屏 */
static bool      s_force_touch_wake = false; /*!< 运行时强制触摸唤醒；不写配置 */
static const void *s_force_touch_wake_owner = NULL;

bool ui_sleep_is_on(void)
{
    return s_on;
}

/* ===================== 自动变暗 ===================== */

static bool dim_enabled(void)
{
    return srv_config_get_int(CFG_DIM_EN, CFG_DEF_DIM_EN) != 0;
}

/* 配置里存的是秒；下限夹到 10 秒，免得设成 0 后一直暗着 */
static int64_t dim_after_us(void)
{
    int sec = (int)srv_config_get_int(CFG_DIM_AFTER, CFG_DEF_DIM_AFTER);
    if (sec < 10) {
        sec = 10;
    }
    return (int64_t)sec * 1000000;
}

/* ===================== 自动锁屏 ===================== */

static bool lock_enabled(void)
{
    return srv_config_get_int(CFG_LOCK_EN, CFG_DEF_LOCK_EN) != 0;
}

/* 配置里存的是秒；下限夹到 30 秒，免得设得太小动不动就跳回主页 */
static int64_t lock_after_us(void)
{
    int sec = (int)srv_config_get_int(CFG_LOCK_AFTER, CFG_DEF_LOCK_AFTER);
    if (sec < 30) {
        sec = 30;
    }
    return (int64_t)sec * 1000000;
}

/**
 * @brief 这些情况算「正在用」，闲置超时不该把用户踢回主页
 *
 * 在播的电台/音乐切页只是把界面藏起来、声音照放，但用户正看着播放控制；
 * 录音切页等于把录音界面丢了；闹钟响着更不能黑屏。
 */
static bool keep_awake(void)
{
    app_page_id_t cur = app_navigator_current();

    if (cur == PAGE_READER && srv_config_get_int(CFG_READ_KEEP_BRIGHT, 0) != 0) {
        return true;                        /* 阅读器开了「不暗屏」：别锁 */
    }

    if (cur == PAGE_RADIO) {
        srv_radio_state_t rs = srv_radio_state();
        if (rs == SRV_RADIO_PLAYING || rs == SRV_RADIO_BUFFERING) {
            return true;
        }
    }
    if (cur == PAGE_MUSIC && srv_player_state() == SRV_PLAYER_PLAYING) {
        return true;
    }
    if (cur == PAGE_RECORDER && srv_recorder_is_active()) {
        return true;
    }

    for (int i = 1; i <= SRV_CONFIG_ALARM_NUM; i++) {
        if (srv_alarm_is_ringing(i)) {
            return true;
        }
    }
    return false;
}

/* 锁屏：清栈回时钟页 + 关屏。跑在 LVGL 定时器里，锁是递归的，重复加没问题 */
static void lock_screen(void)
{
    ESP_LOGI(TAG, "闲置 %d 秒，自动锁屏（回时钟页并关屏）",
             (int)(lock_after_us() / 1000000));

    s_locked = true;

    /* push=false：清掉返回栈，唤醒后按返回键也不会退回锁屏前的页面 */
    app_navigator_switch(PAGE_CLOCK, false);
    ui_sleep_set(true);                     /* 点一下屏幕就回时钟页 */
}

static bool any_pressed(void)
{
    lv_indev_t *indev = NULL;
    while ((indev = lv_indev_get_next(indev)) != NULL) {
        if (lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 变暗后的亮度
 *
 * 按用户设置的比例降（相对亮度），但不会低于 UI_BRIGHTNESS_MIN ——
 * 0 只留给休眠用，界面上的亮度永远是可见的。
 */
static int dim_brightness(void)
{
    int base = (int)srv_config()->brightness;
    int v    = base * DIM_RATIO_PCT / 100;

    if (v < UI_BRIGHTNESS_MIN) {
        v = UI_BRIGHTNESS_MIN;
    }
    if (v > base) {
        v = base;
    }
    return v;
}

/* 已经暗下去了就恢复成用户设置的亮度 */
static void dim_restore(void)
{
    if (!s_dimmed) {
        return;
    }
    s_dimmed = false;
    bsp_display_brightness_set((int)srv_config()->brightness);
    ESP_LOGI(TAG, "亮度恢复 %d%%", (int)srv_config()->brightness);
}

/**
 * @brief 任何触摸都算「有操作」，立刻续期并恢复亮度
 *
 * 必须走触摸事件，不能在定时器里轮询 PRESSED 状态：快速点一下或滑一下
 * 只有几十毫秒，300ms 的轮询很可能一次都抓不到，于是亮度一直暗着回不来。
 */
static bool touch_wake_enabled(void)
{
    return s_force_touch_wake || srv_config_get_int(CFG_TOUCH_WAKE, 1) != 0;
}

void ui_sleep_set_force_touch_wake(bool on, const void *owner)
{
    if (on) {
        s_force_touch_wake = true;
        s_force_touch_wake_owner = owner;
    } else if (owner == NULL || owner == s_force_touch_wake_owner) {
        s_force_touch_wake = false;
        s_force_touch_wake_owner = NULL;
    }
}

static void on_indev_activity(lv_event_t *e)
{
    (void)e;
    /* 休眠中且关了「触摸唤醒」：只当没碰，不续期、不恢复亮度、不解锁。
     * 计时器/倒计时打开了 BOOT 启停时，force 标志会临时接管这里。 */
    if (s_on && !touch_wake_enabled()) {
        return;
    }
    s_last_act_us = esp_timer_get_time();
    s_locked      = false;
    dim_restore();
}

static void dim_tick(lv_timer_t *timer)
{
    (void)timer;

    if (s_on) {
        return;                     /* 休眠中由休眠自己管亮度，也别重复锁 */
    }

    int64_t now = esp_timer_get_time();

    /* 阅读器里开了「不暗屏」就一直算「有操作」；
     * 手指按着也算，这样长按拖动不会被误判成闲置。 */
    bool hold = (app_navigator_current() == PAGE_READER &&
                 srv_config_get_int(CFG_READ_KEEP_BRIGHT, 0) != 0);

    if (hold || any_pressed()) {
        s_last_act_us = now;
        s_locked      = false;
        dim_restore();
        return;
    }

    /* 先变暗 */
    if (dim_enabled()) {
        if (!s_dimmed && (now - s_last_act_us) >= dim_after_us()) {
            s_dimmed = true;
            bsp_display_brightness_set(dim_brightness());
            ESP_LOGI(TAG, "闲置 %d 秒，亮度降到 %d%%",
                     (int)(dim_after_us() / 1000000), dim_brightness());
        }
    } else {
        /* 关掉自动变暗后，若此刻正暗着就把亮度还回去 */
        dim_restore();
    }

    /* 再锁屏：比变暗晚一档，且正在听/录/响铃时不锁 */
    if (lock_enabled() && !s_locked &&
        (now - s_last_act_us) >= lock_after_us() && !keep_awake()) {
        lock_screen();
    }
}

void ui_sleep_set(bool on)
{
    if (on == s_on || s_layer == NULL) {
        return;
    }

    if (bsp_display_lock(UI_LOCK_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "取 LVGL 锁失败，休眠状态没切成");
        return;
    }

    s_on = on;

    /* 不管睡还是醒，都从这个时刻重新计时，唤醒后不会立刻又变暗 */
    s_dimmed      = false;
    s_last_act_us = esp_timer_get_time();

    if (on) {
        /* 屏幕方向可能和初始化时不同（旋转后在别的页面里休眠），
         * 休眠层要正好盖住当前逻辑屏幕 */
        lv_obj_set_size(s_layer, ui_scr_w(), ui_scr_h());
        lv_obj_remove_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
    }

    bsp_display_unlock();

    /* 背光是 I2C 操作，放在 LVGL 锁外面做。
     * 亮度 0 只在这里用 —— 界面上允许调到的最低值是 UI_BRIGHTNESS_MIN。 */
    bsp_display_brightness_set(on ? 0 : (int)srv_config()->brightness);
    ESP_LOGI(TAG, "%s", on ? "已休眠（关屏）" : "已唤醒");
}

/* 睡着的时候用户第一反应是戳屏幕，所以点哪都醒；
 * 关掉「触摸唤醒」后这一下不亮屏，只能按 BOOT 键 */
static void on_layer_click(lv_event_t *e)
{
    (void)e;
    if (!touch_wake_enabled()) {
        return;
    }
    ui_sleep_set(false);
}

/* 跑在 srv_input 的轮询任务上下文：不能直接碰 LVGL，ui_sleep_set 内部会加锁 */
static void on_boot_key(srv_input_evt_t evt, void *arg)
{
    (void)arg;

    /* 响铃时 BOOT 键优先停铃，别把屏幕关了听闹钟 */
    for (int i = 1; i <= SRV_CONFIG_ALARM_NUM; i++) {
        if (srv_alarm_is_ringing(i)) {
            srv_alarm_stop(i);
            ESP_LOGI(TAG, "BOOT 键停止闹钟 %d", i);
            return;
        }
    }

    /* 计时器 / 倒计时页打开了 BOOT 启停时，短按交给对应页面处理。
     * 这些页面同时会设置运行时触摸唤醒覆盖，所以屏幕仍可用触摸唤醒。 */
    if (evt == SRV_INPUT_EVT_SHORT_PRESS || evt == SRV_INPUT_EVT_DOUBLE_CLICK) {
        app_page_id_t page = app_navigator_current();
        if (page == PAGE_TIMER && ui_timer_boot_control_enabled()) {
            s_last_act_us = esp_timer_get_time();
            ui_timer_boot_toggle();
            return;
        }
        if (page == PAGE_COUNTDOWN && ui_countdown_boot_control_enabled()) {
            s_last_act_us = esp_timer_get_time();
            ui_countdown_boot_toggle();
            return;
        }
    }

    ui_sleep_set(!s_on);
}

void ui_sleep_init(void)
{
    /* 挂在 LVGL 顶层：不受页面切换影响，也盖得住所有控件 */
    s_layer = lv_obj_create(lv_layer_top());
    lv_obj_remove_flag(s_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_layer, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(s_layer, 0, 0);
    lv_obj_set_style_bg_color(s_layer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_layer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_layer, 0, 0);
    lv_obj_set_style_radius(s_layer, 0, 0);
    lv_obj_set_style_pad_all(s_layer, 0, 0);
    lv_obj_add_flag(s_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_layer, on_layer_click, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_layer, LV_OBJ_FLAG_HIDDEN);

    s_on = false;
    s_dimmed = false;
    s_locked = false;
    s_force_touch_wake = false;
    s_force_touch_wake_owner = NULL;
    s_last_act_us = esp_timer_get_time();

    /* BOOT 键统一在这里处理（srv_input 是单回调槽，闹钟那边不再抢） */
    srv_input_register_cb(on_boot_key, NULL);

    /* 触摸即续期：按下和松手都记一次，短暂点击也不会漏掉 */
    lv_indev_t *indev = NULL;
    while ((indev = lv_indev_get_next(indev)) != NULL) {
        lv_indev_add_event_cb(indev, on_indev_activity, LV_EVENT_PRESSED, NULL);
        lv_indev_add_event_cb(indev, on_indev_activity, LV_EVENT_RELEASED, NULL);
    }

    /* 盯着「有没有操作」：闲置超过设定时间就按比例压低亮度 */
    lv_timer_create(dim_tick, DIM_TICK_MS, NULL);

    ESP_LOGI(TAG, "休眠就绪：BOOT 键关屏/唤醒；自动变暗 %s（闲置 %d 秒）；自动锁屏 %s（闲置 %d 秒）",
             dim_enabled() ? "开" : "关",
             (int)(dim_after_us() / 1000000),
             lock_enabled() ? "开" : "关",
             (int)(lock_after_us() / 1000000));
}