#include "ui_clock.h"
#include "ui_common.h"
#include "ui_icons.h"
#include "srv_rtc.h"
#include "srv_sd.h"
#include "srv_sensors.h"
#include "srv_battery.h"
#include "srv_wifi.h"
#include "srv_timer.h"
#include "srv_player.h"
#include "srv_audio.h"
#include "srv_safe.h"
#include <stdio.h>
#include <string.h>

static const char *WEEK_CN[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};

static lv_obj_t *s_time_label;
static lv_obj_t *s_sec_label;
static lv_obj_t *s_date_label;
static lv_obj_t *s_temp_label;
static lv_obj_t *s_humi_label;
static lv_obj_t *s_timer_label;
/* 初值取 4：第一秒就立刻刷一次电池/温湿度，不用等满 5 秒 */
static int s_refresh_cnt = 4;

/* ===== 后台播放控制条（只有音乐在放的时候才露面） ===== */
#define PCTL_BTN_W  48
#define PCTL_BTN_H  28

static lv_obj_t *s_pctl       = NULL;   /*!< 整条 */
static lv_obj_t *s_pctl_play  = NULL;   /*!< 播放/暂停键上的图标 */
static lv_obj_t *s_pctl_vol   = NULL;   /*!< 音量滑条 */
static bool      s_pctl_shown = false;

/* ===================== 后台播放控制条 ===================== */

/* 已经按过停止、但播放任务还没收尾：这段时间强制隐藏，避免闪一下又出来 */
static bool s_pctl_stopping = false;

/* 当前挂的是不是「暂停」图标：只在真的变了才 set_src，避免每秒无谓重绘 */
static bool s_pctl_pause_icon = false;

static void pctl_set_icon(bool want_pause)
{
    if (want_pause == s_pctl_pause_icon) {
        return;
    }
    s_pctl_pause_icon = want_pause;
    lv_image_set_src(s_pctl_play, want_pause ? &ic_pause : &ic_play);
}

static void pctl_sync(void)
{
    bool on = (srv_player_state() != SRV_PLAYER_IDLE);

    if (s_pctl_stopping) {
        if (on) {
            on = false;             /* 停的请求还在队列里，先当没停 */
        } else {
            s_pctl_stopping = false;
        }
    }

    if (on != s_pctl_shown) {
        s_pctl_shown = on;
        if (on) {
            lv_obj_remove_flag(s_pctl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_pctl, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!on) {
        return;
    }

    /* 播放中挂「暂停」图标（提示点它可以暂停），暂停/停止挂「播放」 */
    pctl_set_icon(srv_player_state() == SRV_PLAYER_PLAYING);

    /* 音量可能在设置里被改过，同步回来（拖动中不打断） */
    if (!lv_slider_is_dragged(s_pctl_vol)) {
        lv_slider_set_value(s_pctl_vol, srv_audio_get_volume(), LV_ANIM_OFF);
    }
}

static void on_pctl_prev(lv_event_t *e)
{
    (void)e;
    (void)srv_player_prev();
}

static void on_pctl_next(lv_event_t *e)
{
    (void)e;
    (void)srv_player_next();
}

static void on_pctl_play(lv_event_t *e)
{
    (void)e;

    bool playing = (srv_player_state() == SRV_PLAYER_PLAYING);
    (void)srv_player_toggle();

    /* 命令是投给播放任务的，state 要过一会儿才变；
     * 先按预期把图标翻过来，下一秒的 pctl_sync 会按真实状态校正。 */
    pctl_set_icon(!playing);
}

static void on_pctl_stop(lv_event_t *e)
{
    (void)e;
    s_pctl_stopping = true;
    (void)srv_player_stop();

    s_pctl_shown = false;
    lv_obj_add_flag(s_pctl, LV_OBJ_FLAG_HIDDEN);
}

static void on_pctl_vol(lv_event_t *e)
{
    (void)e;
    (void)srv_audio_set_volume((int)lv_slider_get_value(s_pctl_vol));
}

/* 控制条上的一个键；返回图标对象（播放键要换图标，所以留出来） */
static lv_obj_t *pctl_btn(lv_obj_t *parent, lv_coord_t x, const lv_image_dsc_t *icon,
                          bool upside_down, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, PCTL_BTN_W, PCTL_BTN_H);
    lv_obj_set_pos(btn, x, 3);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, UI_COLOR_BG, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    ui_add_click_feedback(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ic = lv_image_create(btn);
    lv_image_set_src(ic, icon);
    lv_obj_set_style_image_recolor(ic, UI_COLOR_TEXT, 0);
    lv_obj_set_style_radius(ic, 0, 0);
    if (upside_down) {
        /* 没有「下一曲」图标：拿上一曲的图标转 180 度 */
        lv_image_set_pivot(ic, icon->header.w / 2, icon->header.h / 2);
        lv_image_set_rotation(ic, 1800);
    }
    lv_obj_center(ic);
    return ic;
}

static void pctl_create(lv_obj_t *root)
{
    s_pctl = lv_obj_create(root);
    lv_obj_remove_flag(s_pctl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_pctl, 308, 34);
    lv_obj_align(s_pctl, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(s_pctl, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_pctl, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pctl, 0, 0);
    lv_obj_set_style_radius(s_pctl, 8, 0);
    lv_obj_set_style_pad_all(s_pctl, 0, 0);
    lv_obj_add_flag(s_pctl, LV_OBJ_FLAG_HIDDEN);

    s_pctl_vol = lv_slider_create(s_pctl);
    lv_obj_set_size(s_pctl_vol, 80, 6);
    lv_obj_align(s_pctl_vol, LV_ALIGN_LEFT_MID, 8, 0);
    lv_slider_set_range(s_pctl_vol, 0, 100);
    lv_slider_set_value(s_pctl_vol, srv_audio_get_volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_pctl_vol, UI_COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_pctl_vol, UI_COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_pctl_vol, UI_COLOR_PRIMARY, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_pctl_vol, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(s_pctl_vol, on_pctl_vol, LV_EVENT_VALUE_CHANGED, NULL);

    (void)pctl_btn(s_pctl, 96, &ic_prev, false, on_pctl_prev);
    s_pctl_play = pctl_btn(s_pctl, 150, &ic_play, false, on_pctl_play);
    (void)pctl_btn(s_pctl, 204, &ic_prev, true, on_pctl_next);
    (void)pctl_btn(s_pctl, 258, &ic_stop, false, on_pctl_stop);

    s_pctl_shown = false;
    s_pctl_stopping = false;
    s_pctl_pause_icon = false;      /* 初始挂的是「播放」图标 */
}

/**
 * @brief 计时器 / 倒计时运行提示行
 *
 * 两者都没有时整行隐藏；只有秒表就只显示秒表，反之亦然。
 */
static void timer_line_refresh(void)
{
    bool sw_on = srv_timer_sw_is_active();
    bool cd_on = srv_timer_cd_is_active() || srv_timer_cd_is_finished();

    if (!sw_on && !cd_on) {
        lv_obj_add_flag(s_timer_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    char   buf[64];
    size_t p = 0;

    if (sw_on) {
        int64_t sec = srv_timer_sw_elapsed_ms() / 1000;
        p += (size_t)snprintf(buf + p, sizeof(buf) - p, "计时 %02d:%02d",
                              (int)(sec / 60), (int)(sec % 60));
    }
    if (sw_on && cd_on) {
        p += (size_t)snprintf(buf + p, sizeof(buf) - p, "  ");
    }
    if (cd_on) {
        if (srv_timer_cd_is_finished()) {
            snprintf(buf + p, sizeof(buf) - p, "倒计 时间到");
        } else {
            int64_t sec = (srv_timer_cd_remaining_ms() + 999) / 1000;
            snprintf(buf + p, sizeof(buf) - p, "倒计 %02d:%02d:%02d",
                     (int)(sec / 3600), (int)((sec / 60) % 60), (int)(sec % 60));
        }
    }

    lv_obj_remove_flag(s_timer_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_timer_label, buf);
}

/* ===================== 网络状态指示 ===================== */

/*! 左上角 SD 图标右边的小信号柱：亮几格表示网络状态。
 *
 *  信息直接取自 srv_wifi 的三个状态位，不额外去查 RSSI —— 床头钟上只要
 *  一眼看出「连上了 / 在连 / 没连」就够了，画真实信号强度反而要多一次
 *  驱动查询，收益不成正比。
 *
 *  WiFi 整个关掉时这块整体隐藏，不在界面上留一个永远灰着的死图标。 */
#define WIFI_BAR_N    3
#define WIFI_BAR_W    4
#define WIFI_BAR_GAP  3
#define WIFI_BAR_H0   5      /*!< 第一格高度 */
#define WIFI_BAR_DH   3      /*!< 每格递增高度 */

static lv_obj_t *s_wifi_box;
static lv_obj_t *s_wifi_bars[WIFI_BAR_N];

static void wifi_indicator_refresh(void)
{
    if (s_wifi_box == NULL) {
        return;
    }

    if (!srv_wifi_is_enabled()) {
        lv_obj_add_flag(s_wifi_box, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_wifi_box, LV_OBJ_FLAG_HIDDEN);

    int        lit;
    lv_color_t color;
    if (srv_wifi_is_connected()) {
        lit   = 3;
        color = UI_COLOR_OK;          /* 绿：已连上 */
    } else if (srv_wifi_is_connecting()) {
        lit   = 2;
        color = UI_COLOR_WARN;        /* 黄：正在连 */
    } else {
        lit   = 1;
        color = UI_COLOR_TEXT_DIM;    /* 灰：开着但没连 */
    }

    for (int i = 0; i < WIFI_BAR_N; i++) {
        lv_obj_set_style_bg_color(s_wifi_bars[i], color, 0);
        lv_obj_set_style_bg_opa(s_wifi_bars[i], (i < lit) ? LV_OPA_COVER : LV_OPA_30, 0);
    }
}

static void wifi_indicator_create(lv_obj_t *parent, lv_obj_t *anchor)
{
    const lv_coord_t max_h = WIFI_BAR_H0 + (WIFI_BAR_N - 1) * WIFI_BAR_DH;

    s_wifi_box = lv_obj_create(parent);
    lv_obj_remove_flag(s_wifi_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_wifi_box, WIFI_BAR_N * WIFI_BAR_W + (WIFI_BAR_N - 1) * WIFI_BAR_GAP, max_h);
    if (anchor != NULL) {
        /* 挂在 SD 图标右边，而不是硬编码坐标：ic_SD 是 32x32，凭估计写 x 很容易压上去，
         * 而且以后换图标尺寸这里也不用跟着改。SD 图标隐藏时 coords 依然有效，对齐照常。 */
        lv_obj_align_to(s_wifi_box, anchor, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
    } else {
        lv_obj_align(s_wifi_box, LV_ALIGN_TOP_LEFT, 48, 15);
    }
    lv_obj_set_style_bg_opa(s_wifi_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wifi_box, 0, 0);
    lv_obj_set_style_radius(s_wifi_box, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_box, 0, 0);
    lv_obj_add_flag(s_wifi_box, LV_OBJ_FLAG_HIDDEN);      /* 等第一次刷新再决定 */

    for (int i = 0; i < WIFI_BAR_N; i++) {
        const lv_coord_t h = WIFI_BAR_H0 + i * WIFI_BAR_DH;
        lv_obj_t *bar = lv_obj_create(s_wifi_box);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(bar, WIFI_BAR_W, h);
        lv_obj_set_pos(bar, i * (WIFI_BAR_W + WIFI_BAR_GAP), max_h - h);   /* 底边对齐 */
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 1, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_30, 0);
        s_wifi_bars[i] = bar;
    }
}

static void clock_refresh_timer(lv_timer_t *timer)
{
    (void)timer;
    srv_rtc_time_t t;
    char buf[48];

    if (srv_rtc_get_time(&t) != ESP_OK) {
        return;
    }

    /* 大号时:分 + 秒 */
    snprintf(buf, sizeof(buf), "%02d:%02d", t.hour, t.min);
    lv_label_set_text(s_time_label, buf);

    snprintf(buf, sizeof(buf), "%02d", t.sec);
    lv_label_set_text(s_sec_label, buf);

    /* 日期 + 星期 */
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %s",
             t.year, t.month, t.day, WEEK_CN[(t.dotw + 7) % 7]);
    lv_label_set_text(s_date_label, buf);

    /* 状态栏：SD 卡有卡才显示图标 */
    ui_sd_icon_set(srv_sd_is_mounted());

    /* 状态栏：WiFi 信号柱（关网时整块隐藏） */
    wifi_indicator_refresh();

    /* 计时器 / 倒计时提示行 */
    timer_line_refresh();

    /* 后台播放控制条：音乐在放就露面 */
    pctl_sync();

    /* 状态栏：电池图标（代码绘制）；温湿度每 5 秒读一次传感器 */
    if (++s_refresh_cnt >= 5) {
        s_refresh_cnt = 0;

        float vol = 0.0f;
        int   pct = 0;
        if (srv_battery_get(&vol, &pct) == ESP_OK) {
            ui_battery_set(pct, true);
        } else {
            ui_battery_set(0, false);
        }

        float temp = 0.0f, humi = 0.0f;
        if (srv_sensors_get_temp_humi(&temp, &humi) == ESP_OK) {
            snprintf(buf, sizeof(buf), "%.1f℃", temp);
            lv_label_set_text(s_temp_label, buf);
            snprintf(buf, sizeof(buf), "%.1f%%RH", humi);
            lv_label_set_text(s_humi_label, buf);
        } else {
            lv_label_set_text(s_temp_label, "--℃");
            lv_label_set_text(s_humi_label, "--%RH");
        }
    }
}

/*! 左上角热区被点：交给 srv_safe 数次数，够了就等右滑（右滑在 ui_common 里处理） */
static void on_safe_tap(lv_event_t *e)
{
    (void)e;
    srv_safe_tap();
}

/**
 * @brief 页面销毁时删掉秒级定时器
 *
 * 必须这么做：本页的标签指针是 static，页面被 lv_obj_delete 之后它们立刻变成野指针，
 * 定时器若继续跑就会对已释放的 lv_obj_t 调 lv_label_set_text，
 * 表现为 Store access fault（实测崩在 lv_label_revert_dots 里写 label->text）。
 * 定时器句柄通过事件 user_data 传入，不能用 static 变量：
 * 新页面可能先于旧页面的销毁回调创建，static 会被覆盖成新定时器。
 */
static void clock_root_del_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    lv_timer_t *t = (lv_timer_t *)lv_event_get_user_data(e);
    if (t != NULL) {
        lv_timer_delete(t);
    }

    /* 控制条随页面一起没了，指针要清掉，否则新页面建好前定时器可能碰到野指针 */
    s_pctl            = NULL;
    s_pctl_play       = NULL;
    s_pctl_vol        = NULL;
    s_pctl_shown      = false;
    s_pctl_stopping   = false;
    s_pctl_pause_icon = false;

    /* 网络指示器同样跟着页面一起销毁 */
    s_wifi_box = NULL;
    memset(s_wifi_bars, 0, sizeof(s_wifi_bars));
}

void ui_clock_create(lv_obj_t *root)
{
    /* ===== 左上角：安全模式入口热区 =====
     * 完全透明，压在 SD 图标那一块上，看不出是可点的。
     * 连点 5 次 + 2 秒内右滑切换安全模式（见 srv_safe / ui_common 的右滑处理）。 */
    lv_obj_t *safe_hot = lv_obj_create(root);
    lv_obj_remove_flag(safe_hot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(safe_hot, 46, 40);
    lv_obj_set_pos(safe_hot, 0, 0);
    lv_obj_set_style_bg_opa(safe_hot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(safe_hot, 0, 0);
    lv_obj_set_style_radius(safe_hot, 0, 0);
    lv_obj_set_style_pad_all(safe_hot, 0, 0);
    lv_obj_add_event_cb(safe_hot, on_safe_tap, LV_EVENT_CLICKED, NULL);

    /* ===== 状态栏 ===== */
    /* SD：有卡显示图标，没卡直接不显示 */
    lv_obj_t *sd_img = ui_sd_icon_create(root, LV_ALIGN_TOP_LEFT, 10, 5);

    /* WiFi：信号柱，挂在 SD 图标右侧 */
    wifi_indicator_create(root, sd_img);

    /* 电池：代码绘制（外壳 + 正极 + 电量条 + 百分比） */
    ui_battery_create(root, LV_ALIGN_TOP_RIGHT, -10, 5);

    /* ===== 中央大字时间 ===== */
    s_time_label = lv_label_create(root);
    lv_obj_set_style_text_color(s_time_label, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_time_label, &font_clock_48, 0);
    lv_label_set_text(s_time_label, "00:00");
    lv_obj_align(s_time_label, LV_ALIGN_CENTER, 0, -30);

    /* 秒：放在时间右侧外侧，避免与时分重叠 */
    s_sec_label = lv_label_create(root);
    lv_obj_set_style_text_color(s_sec_label, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_sec_label, &font_ui_14, 0);
    lv_label_set_text(s_sec_label, "00");
    lv_obj_align_to(s_sec_label, s_time_label, LV_ALIGN_OUT_RIGHT_MID, 6, 12);

    /* ===== 日期 + 星期 ===== */
    s_date_label = lv_label_create(root);
    lv_obj_set_style_text_color(s_date_label, UI_COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_date_label, &font_ui_14, 0);
    lv_label_set_text(s_date_label, "----");
    lv_obj_align(s_date_label, LV_ALIGN_CENTER, 0, 26);

    /* ===== 计时器 / 倒计时（运行时才显示） ===== */
    s_timer_label = lv_label_create(root);
    lv_obj_set_style_text_color(s_timer_label, UI_COLOR_WARN, 0);
    lv_obj_set_style_text_font(s_timer_label, &font_ui_14, 0);
    lv_label_set_text(s_timer_label, "");
    lv_obj_align(s_timer_label, LV_ALIGN_CENTER, 0, 48);
    lv_obj_add_flag(s_timer_label, LV_OBJ_FLAG_HIDDEN);
    timer_line_refresh();

    /* ===== 温湿度 ===== */
    s_temp_label = lv_label_create(root);
    lv_obj_set_style_text_color(s_temp_label, UI_COLOR_TEMP, 0);
    lv_obj_set_style_text_font(s_temp_label, &font_ui_14, 0);
    lv_label_set_text(s_temp_label, "--℃");
    lv_obj_align(s_temp_label, LV_ALIGN_CENTER, -45, 70);

    s_humi_label = lv_label_create(root);
    lv_obj_set_style_text_color(s_humi_label, UI_COLOR_HUMI, 0);
    lv_obj_set_style_text_font(s_humi_label, &font_ui_14, 0);
    lv_label_set_text(s_humi_label, "--%RH");
    lv_obj_align(s_humi_label, LV_ALIGN_CENTER, 45, 70);

    /* ===== 后台播放控制条（默认隐藏） ===== */
    pctl_create(root);
    pctl_sync();

    /* ===== 每秒刷新（随页面销毁自动删除） ===== */
    lv_timer_t *tmr = lv_timer_create(clock_refresh_timer, 1000, NULL);
    lv_obj_add_event_cb(root, clock_root_del_cb, LV_EVENT_DELETE, tmr);

    /* 页面一建好就主动刷一次：不能等 1 秒后的第一次定时器，否则从别的页面
     * 返回时先看到 "00:00"、日期 "----"、温湿度 "--"，要等一秒才恢复。
     * s_refresh_cnt 置 4 让这次调用顺便把电量/温湿度也读一遍（下一拍归零，
     * 之后仍按 5 秒周期）；页面重建时 static 里残留的计数也一并覆盖掉。 */
    s_refresh_cnt = 4;
    clock_refresh_timer(NULL);
}
