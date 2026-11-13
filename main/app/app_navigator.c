#include "app_navigator.h"
#include "bsp/esp32_c5_touch_lcd_2.8.h"
#include "srv_config.h"
#include "ui_common.h"
#include "ui_clock.h"
#include "ui_home.h"
#include "ui_func.h"
#include "ui_calculator.h"
#include "ui_settings.h"
#include "ui_set_pages.h"
#include "ui_timer.h"
#include "ui_countdown.h"
#include "ui_recorder.h"
#include "ui_photos.h"
#include "ui_words.h"
#include "ui_word_forms.h"
#include "ui_word_study.h"
#include "ui_files.h"
#include "ui_radio.h"
#include "ui_ai.h"
#include "ui_calendar.h"
#include "ui_safe_menu.h"
#include "ui_bookshelf.h"
#include "ui_reader.h"
#include "ui_music.h"
#include "ui_games.h"
#include "ui_game_2048.h"
#include "ui_game_snake.h"
#include "ui_game_gomoku.h"
#include "ui_game_paint.h"
#include "ui_game_ball.h"
#include "ui_game_soccer.h"
#include "esp_log.h"

#define NAV_STACK_MAX   8

static const char *TAG = "app_nav";

/* 切页动画方向：从哪来到哪去 */
typedef enum {
    ANIM_FROM_RIGHT,    /*!< 进入下级页：从右侧滑入 */
    ANIM_FROM_LEFT,     /*!< 返回上级页：从左侧滑入 */
    ANIM_FROM_BOTTOM,   /*!< 从下往上滑入（日历） */
    ANIM_NONE,          /*!< 首次进入，不做动画 */
} nav_anim_t;

static const app_page_ops_t s_page_ops[PAGE_MAX] = {
    [PAGE_CLOCK]     = {.build = ui_clock_create},
    [PAGE_HOME]      = {.build = ui_home_create},
    [PAGE_FUNC]      = {.build = ui_func_create},
    [PAGE_SETTINGS]  = {.build = ui_settings_create},
    [PAGE_SET_NET]   = {.build = ui_set_net_create},
    [PAGE_SET_NET_ADD] = {.build = ui_set_net_add_create},
    [PAGE_SET_TIME]  = {.build = ui_set_time_create},
    [PAGE_SET_RECORD] = {.build = ui_set_record_create},
    [PAGE_SET_ALARM] = {.build = ui_set_alarm_create},
    [PAGE_SET_READ]  = {.build = ui_set_read_create},
    [PAGE_SET_MUSIC] = {.build = ui_set_music_create},
    [PAGE_SET_BRIGHT] = {.build = ui_set_bright_create},
    [PAGE_SET_ABOUT] = {.build = ui_set_about_create},
    [PAGE_SET_BATTERY] = {.build = ui_set_battery_create},
    [PAGE_SET_PHOTO] = {.build = ui_set_photo_create},
    [PAGE_FILES]     = {.build = ui_files_create},
    [PAGE_CALENDAR]  = {.build = ui_calendar_create},
    [PAGE_SAFE_MENU] = {.build = ui_safe_menu_create},
    [PAGE_SETTINGS_FULL] = {.build = ui_settings_full_create},
    [PAGE_TIMER]     = {.build = ui_timer_create},
    [PAGE_COUNTDOWN] = {.build = ui_countdown_create},
    [PAGE_RECORDER]  = {.build = ui_recorder_create},
    [PAGE_PHOTOS]    = {.build = ui_photos_create},
    [PAGE_WORDS]     = {.build = ui_words_create},
    [PAGE_WORD_FORMS] = {.build = ui_word_forms_create},
    [PAGE_WORD_STUDY] = {.build = ui_word_study_create},
    [PAGE_RADIO]     = {.build = ui_radio_create},
    [PAGE_AI]        = {.build = ui_ai_create},
    [PAGE_CALCULATOR] = {.build = ui_calculator_create},
    [PAGE_BOOKSHELF] = {.build = ui_bookshelf_create},
    [PAGE_READER]    = {.build = ui_reader_create},
    [PAGE_MUSIC]     = {.build = ui_music_create},
    [PAGE_GAMES]     = {.build = ui_games_create},
    [PAGE_GAME_2048] = {.build = ui_game_2048_create},
    [PAGE_GAME_SNAKE]  = {.build = ui_game_snake_create},
    [PAGE_GAME_GOMOKU] = {.build = ui_game_gomoku_create},
    [PAGE_GAME_PAINT]  = {.build = ui_game_paint_create},
    [PAGE_GAME_BALL]   = {.build = ui_game_ball_create},
    [PAGE_GAME_SOCCER] = {.build = ui_game_soccer_create},
};

static app_page_id_t s_stack[NAV_STACK_MAX];
static int           s_depth   = 0;
static app_page_id_t s_current = PAGE_CLOCK;
static lv_obj_t     *s_screen  = NULL;

/* 运行时方向：重启后从 srv_config 恢复；所有可旋转页面共用这一个值。
 * 页面切换前先按它设置硬件/逻辑方向。时钟、日历、游戏页锁定横屏。 */
static uint16_t s_rot = 0;

/*! 正在构建的页面（页面构建期间给标题栏判断要不要放旋转键用） */
static app_page_id_t s_building = PAGE_MAX;

/* 这些页面自己要用横向滑动（翻页 / 操作游戏 / 涂鸦 / 拨球 / 切照片），
 * 不能再兼做「右滑返回」，否则两个动作会抢同一个手势。它们的退出都走标题栏返回键。 */
static bool page_has_own_gesture(app_page_id_t page)
{
    switch (page) {
    case PAGE_READER:
    case PAGE_PHOTOS:
    case PAGE_GAME_2048:
    case PAGE_GAME_SNAKE:
    case PAGE_GAME_GOMOKU:
    case PAGE_GAME_PAINT:
    case PAGE_GAME_SOCCER:
        return true;
    default:
        return false;
    }
}

/* 页面构建：屏幕 -> 根容器 -> 页面控件。
 * 关键点：屏幕只负责提供不透明底色，页面内容与滑入动画都作用在根容器上。
 * 直接给屏幕设 LV_PCT 尺寸或做位移动画会导致重绘区域算错，出现旧界面残影。 */
static lv_obj_t *page_build(app_page_id_t page)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    lv_obj_t *root = ui_page_root_create(scr);

    /* 构建期间让标题栏知道「这一页能不能转」，决定右端要不要放旋转键 */
    s_building = page;
    if (s_page_ops[page].build != NULL) {
        s_page_ops[page].build(root);
    } else {
        /* 尚未实现的页面用占位内容兜底，保持导航结构可测 */
        ui_placeholder_build(root, "待实现", "该功能尚未实现",
                             app_navigator_back_cb, NULL);
    }
    s_building = PAGE_MAX;

    /* 各页面统一个手势入口（自己要用滑动的页面除外） */
    if (!page_has_own_gesture(page)) {
        ui_enable_swipe(root);
    }

    return scr;
}

static uint16_t normalize_rotation(int32_t deg)
{
    return (deg == 270) ? 270 : 0;
}

/* 时钟页是床头钟，日历和游戏页都按固定的横屏像素排版：这三类不参与旋转。
 * 其余页面统一跟随全局方向。 */
static bool page_can_rotate(app_page_id_t page)
{
    switch (page) {
    case PAGE_CLOCK:
    case PAGE_CALENDAR:
    case PAGE_GAMES:
    case PAGE_GAME_2048:
    case PAGE_GAME_SNAKE:
    case PAGE_GAME_GOMOKU:
    case PAGE_GAME_PAINT:
    case PAGE_GAME_BALL:
    case PAGE_GAME_SOCCER:
        return false;
    default:
        return true;
    }
}

static void rotation_state_load(void)
{
    s_rot = normalize_rotation(srv_config_get_int(CFG_ROT, CFG_DEF_ROT));
    ESP_LOGI(TAG, "加载全局页面方向：%u°", (unsigned)s_rot);
}

bool app_navigator_page_can_rotate(app_page_id_t page)
{
    if (page >= PAGE_MAX) {
        return false;
    }
    return page_can_rotate(page);
}

bool app_navigator_rotation_allowed(void)
{
    return page_can_rotate((s_building != PAGE_MAX) ? s_building : s_current);
}

/* 创建并切换页面（调用前必须已持有 LVGL 锁）。
 * 进页前先设置该页应使用的方向，再构建页面，保证首次绘制就是正确逻辑尺寸；
 * 可旋转页面用全局方向，锁横屏页面固定 0°。 */
static void switch_locked_ex(app_page_id_t page, nav_anim_t anim, bool preserve_rotation)
{
    uint16_t desired = preserve_rotation ? bsp_display_get_rotation()
                                         : (page_can_rotate(page) ? s_rot : 0);
    if (bsp_display_get_rotation() != desired) {
        (void)bsp_display_set_rotation(desired);
    }

    lv_obj_t *scr = page_build(page);
    if (scr == NULL) {
        ESP_LOGE(TAG, "页面 %d 创建失败", (int)page);
        return;
    }

    lv_obj_t *old = s_screen;
    lv_screen_load(scr);
    s_screen = scr;

    lv_obj_t *root = lv_obj_get_child(scr, 0);
    if (anim != ANIM_NONE && root != NULL) {
        ui_slide_in_dir(root, (anim == ANIM_FROM_RIGHT) ? UI_SLIDE_FROM_RIGHT
                                : (anim == ANIM_FROM_BOTTOM) ? UI_SLIDE_FROM_BOTTOM
                                                             : UI_SLIDE_FROM_LEFT);
    }

    /* 删除旧屏：之前只 load 不删除，每次切页都泄漏一个屏幕对象。
     * 必须用 delete_async —— 切页常由旧屏上按钮的点击回调触发，
     * 同步删除会把"正在派发事件的对象"的祖先立刻释放掉，
     * 回调返回后 LVGL 再访问该对象就是 use-after-free。 */
    if (old != NULL && lv_obj_is_valid(old)) {
        lv_obj_delete_async(old);
    }

    s_current = page;
}

static void switch_locked(app_page_id_t page, nav_anim_t anim)
{
    switch_locked_ex(page, anim, false);
}

void app_navigator_init(void)
{
    s_depth   = 0;
    s_current = PAGE_CLOCK;
    s_screen  = NULL;
    rotation_state_load();

    if (bsp_display_lock(UI_LOCK_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGE(TAG, "获取 LVGL 锁失败，界面未初始化");
        return;
    }

    ui_common_init();
    switch_locked(PAGE_CLOCK, ANIM_NONE);

    bsp_display_unlock();
}

void app_navigator_switch(app_page_id_t page, bool push)
{
    app_navigator_switch_anim(page, push, APP_NAV_ANIM_AUTO);
}

void app_navigator_switch_anim(app_page_id_t page, bool push, app_nav_anim_t anim)
{
    if (page >= PAGE_MAX) {
        return;
    }

    if (bsp_display_lock(UI_LOCK_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGE(TAG, "获取 LVGL 锁失败，页面 %d 未切换", (int)page);
        return;
    }

    if (push) {
        if (s_depth < NAV_STACK_MAX) {
            s_stack[s_depth++] = s_current;
        }
        switch_locked(page, (anim == APP_NAV_ANIM_LEFT)   ? ANIM_FROM_LEFT
                            : (anim == APP_NAV_ANIM_BOTTOM) ? ANIM_FROM_BOTTOM
                                                            : ANIM_FROM_RIGHT);
    } else {
        s_depth = 0;
        switch_locked(page, (anim == APP_NAV_ANIM_BOTTOM) ? ANIM_FROM_BOTTOM : ANIM_FROM_LEFT);
    }

    bsp_display_unlock();
}

void app_navigator_back(void)
{
    if (bsp_display_lock(UI_LOCK_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGE(TAG, "获取 LVGL 锁失败，返回失败");
        return;
    }

    app_page_id_t prev = (s_depth > 0) ? s_stack[--s_depth] : PAGE_CLOCK;
    switch_locked(prev, ANIM_FROM_LEFT);

    bsp_display_unlock();
}

void app_navigator_hide(void)
{
    if (bsp_display_lock(UI_LOCK_TIMEOUT_MS) != ESP_OK) {
        return;
    }
    s_depth = 0;
    switch_locked(PAGE_CLOCK, ANIM_FROM_LEFT);
    bsp_display_unlock();
}

void app_navigator_toggle_page_rotation(void)
{
    if (!page_can_rotate(s_current)) {
        return;                     /* 锁横屏的页面没有旋转键，这里只是兜底 */
    }
    if (bsp_display_lock(UI_LOCK_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGE(TAG, "获取 LVGL 锁失败，页面旋转未执行");
        return;
    }

    uint16_t next = (s_rot == 0) ? 270 : 0;
    esp_err_t ret = bsp_display_set_rotation(next);
    if (ret == ESP_OK) {
        s_rot = next;
        (void)srv_config_set_int(CFG_ROT, next, true);
        ESP_LOGI(TAG, "全局页面方向切到 %u° 并已保存", (unsigned)next);
        switch_locked_ex(s_current, ANIM_NONE, true);
    } else {
        ESP_LOGE(TAG, "页面旋转失败: %s", esp_err_to_name(ret));
    }

    bsp_display_unlock();
}

app_page_id_t app_navigator_current(void)
{
    return s_current;
}

app_page_id_t app_navigator_prev(void)
{
    return (s_depth > 0) ? s_stack[s_depth - 1] : PAGE_CLOCK;
}

void app_navigator_back_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        app_navigator_back();
    }
}

void app_navigator_home(void)
{
    if (bsp_display_lock(UI_LOCK_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGE(TAG, "获取 LVGL 锁失败，无法返回主菜单");
        return;
    }
    s_depth = 0;
    switch_locked(PAGE_HOME, ANIM_FROM_LEFT);
    bsp_display_unlock();
}

/*! 长按返回刚处理过的时刻：LVGL 松手时还会补一次 CLICKED，用它把那次吞掉 */
static uint32_t s_back_long_tick;

void app_navigator_back_smart_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    bool fast = (srv_config_get_int(CFG_FAST_HOME, CFG_DEF_FAST_HOME) != 0);

    if (code == LV_EVENT_LONG_PRESSED) {
        s_back_long_tick = lv_tick_get();
        /* 打开「快速返回主页」后长短按互换：长按变成逐级返回 */
        if (fast) {
            app_navigator_back();
        } else {
            app_navigator_home();
        }
        return;
    }

    if (code != LV_EVENT_CLICKED) {
        return;
    }

    /* 长按之后松手补发的那一下不算短按（同阅读器里吞 CLICKED 的做法） */
    if (s_back_long_tick != 0 && lv_tick_elaps(s_back_long_tick) < 800) {
        s_back_long_tick = 0;
        return;
    }

    if (fast) {
        app_navigator_home();
    } else {
        app_navigator_back();
    }
}

void app_navigator_push_placeholder(const char *title, const char *note)
{
    if (bsp_display_lock(UI_LOCK_TIMEOUT_MS) != ESP_OK) {
        return;
    }

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    lv_obj_t *root = ui_page_root_create(scr);
    ui_placeholder_build(root, title, note, app_navigator_back_cb, NULL);
    ui_enable_swipe(root);

    if (s_depth < NAV_STACK_MAX) {
        s_stack[s_depth++] = s_current;
    }

    lv_obj_t *old = s_screen;
    lv_screen_load(scr);
    s_screen = scr;
    ui_slide_in(root, true);           /* 从右侧滑入 */

    if (old != NULL && lv_obj_is_valid(old)) {
        lv_obj_delete_async(old);
    }

    bsp_display_unlock();
}
