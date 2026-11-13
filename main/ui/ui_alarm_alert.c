#include "ui_alarm_alert.h"

#include "ui_common.h"
#include "ui_icons.h"
#include "ui_dialog.h"
#include "ui_sleep.h"
#include "srv_alarm.h"
#include "srv_config.h"

#include "bsp/esp32_c5_touch_lcd_2.8.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>

static const char *TAG = "alarm_alert";

/* 键名与标题走常量表（snprintf 拼 "alarm%d_h" 会被 -Werror=format-truncation 拦） */
static const char *const ALARM_TITLE[SRV_CONFIG_ALARM_NUM] = {"闹钟 1", "闹钟 2", "闹钟 3"};
static const char *const K_H[SRV_CONFIG_ALARM_NUM]         = {"alarm1_h", "alarm2_h", "alarm3_h"};
static const char *const K_M[SRV_CONFIG_ALARM_NUM]         = {"alarm1_m", "alarm2_m", "alarm3_m"};

#define BTN_W       116
#define BTN_H       44

static lv_obj_t   *s_layer;         /*!< 全屏提醒层（挂 lv_layer_top，不随页面销毁） */
static lv_timer_t *s_watch;         /*!< 1Hz 盯着响铃结束 */
static int         s_idx;           /*!< 正在响的闹钟 1..3，0 = 无 */

/* ===================== 全屏层 ===================== */

static void layer_destroy(void)
{
    if (s_layer != NULL && lv_obj_is_valid(s_layer)) {
        lv_obj_delete(s_layer);
    }
    s_layer = NULL;
}

static void on_stop(lv_event_t *e)
{
    (void)e;
    srv_alarm_stop(s_idx);
    layer_destroy();
}

static void on_snooze(lv_event_t *e)
{
    (void)e;
    srv_alarm_snooze(s_idx, 5);
    layer_destroy();
}

/* 底部一个按钮：返回按钮对象 */
static lv_obj_t *make_btn(lv_obj_t *parent, lv_coord_t x, const char *text,
                          lv_color_t color, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, BTN_W, BTN_H);
    lv_obj_set_pos(btn, x, ui_scr_h() - BTN_H - 14);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    ui_add_click_feedback(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lb = lv_label_create(btn);
    lv_obj_set_style_text_font(lb, &font_ui_14, 0);
    lv_obj_set_style_text_color(lb, UI_COLOR_TEXT, 0);
    lv_label_set_text(lb, text);
    lv_obj_center(lb);
    return btn;
}

static void show_ring_layer(int index)
{
    layer_destroy();

    s_layer = lv_obj_create(lv_layer_top());
    lv_obj_remove_flag(s_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_layer, ui_scr_w(), ui_scr_h());
    lv_obj_set_pos(s_layer, 0, 0);
    lv_obj_set_style_bg_color(s_layer, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_layer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_layer, 0, 0);
    lv_obj_set_style_radius(s_layer, 0, 0);
    lv_obj_set_style_pad_all(s_layer, 0, 0);

    /* 标题 */
    lv_obj_t *ttl = lv_label_create(s_layer);
    lv_obj_set_style_text_font(ttl, &font_ui_14, 0);
    lv_obj_set_style_text_color(ttl, UI_COLOR_PRIMARY, 0);
    lv_label_set_text(ttl, ALARM_TITLE[index - 1]);
    lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 18);

    /* 设置的时间，大号数字 */
    char note[16];
    snprintf(note, sizeof(note), "%02d:%02d",
             (int)srv_config_get_int(K_H[index - 1], 0),
             (int)srv_config_get_int(K_M[index - 1], 0));
    lv_obj_t *time_lb = lv_label_create(s_layer);
    lv_obj_set_style_text_font(time_lb, &font_clock_48, 0);
    lv_obj_set_style_text_color(time_lb, UI_COLOR_TEXT, 0);
    lv_label_set_text(time_lb, note);
    lv_obj_align(time_lb, LV_ALIGN_CENTER, 0, -24);

    lv_obj_t *hint = lv_label_create(s_layer);
    lv_obj_set_style_text_font(hint, &font_ui_14, 0);
    lv_obj_set_style_text_color(hint, UI_COLOR_TEXT_DIM, 0);
    lv_label_set_text(hint, "稍后 5 分钟，或停止（BOOT 键同停止）");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 30);

    /* 两个按钮：左稍后，右停止 */
    const lv_coord_t gap = 16;
    const lv_coord_t x0 = (ui_scr_w() - BTN_W * 2 - gap) / 2;
    make_btn(s_layer, x0, "稍后 5 分钟", UI_COLOR_SURFACE, on_snooze);
    make_btn(s_layer, x0 + BTN_W + gap, "停止", UI_COLOR_PRIMARY, on_stop);
}

/* 1Hz：铃停了就收掉全屏层；层被意外删掉（比如切页后重建）而铃还在响，就补弹 */
static void watch_cb(lv_timer_t *t)
{
    if (s_idx == 0 || !srv_alarm_is_ringing(s_idx)) {
        layer_destroy();
        s_idx = 0;
        s_watch = NULL;
        lv_timer_delete(t);
        return;
    }
    if (s_layer == NULL || !lv_obj_is_valid(s_layer)) {
        show_ring_layer(s_idx);
    }
}

/* ===================== 响铃回调 ===================== */

static void alarm_ui_task(void *arg)
{
    int index = (int)(intptr_t)arg;

    /* 先把屏幕点亮：闹钟响时手机一定是亮屏的。ui_sleep_set 自己会取 LVGL 锁，
     * 这里必须在取锁之前调用，避免同线程重复取锁 */
    ui_sleep_set(false);

    if (bsp_display_lock(UI_LOCK_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "取 LVGL 锁失败，跳过闹钟提示");
        vTaskDelete(NULL);
        return;
    }

    s_idx = index;
    show_ring_layer(index);

    if (s_watch == NULL) {
        s_watch = lv_timer_create(watch_cb, 1000, NULL);
    }

    bsp_display_unlock();
    vTaskDelete(NULL);
}

/* srv_alarm 的触发回调：跑在 esp_timer 上下文，不能碰 LVGL，只能转任务 */
static void on_alarm_ring(int index, void *arg)
{
    (void)arg;

    ESP_LOGW(TAG, "闹钟 %d 响铃，弹出全屏提醒", index);

    if (xTaskCreate(alarm_ui_task, "alarm_ui", 4096,
                    (void *)(intptr_t)(index + 1), 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "提示任务创建失败");
    }
}

/* ===================== 初始化 ===================== */

void ui_alarm_alert_init(void)
{
    srv_alarm_register_cb(on_alarm_ring, NULL);

    /* BOOT 键不在这里注册：它是单回调槽，统一交给 ui_sleep 处理
     * （那边会先判响铃、再决定要不要休眠）。 */
    ESP_LOGI(TAG, "闹钟提示就绪（全屏：稍后 5 分钟 / 停止，BOOT 键同停止）");
}
