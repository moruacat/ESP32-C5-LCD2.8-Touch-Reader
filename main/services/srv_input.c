#include "srv_input.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BOOT_GPIO               (GPIO_NUM_28)
#define LONG_PRESS_MS           1000
#define DOUBLE_CLICK_WINDOW_MS  400
#define POLL_INTERVAL_MS        20

static const char *TAG = "srv_input";

static srv_input_cb_t s_cb = NULL;
static void *s_cb_arg = NULL;

static int64_t s_press_start = 0;      /*!< 按下时刻 us */
static bool s_is_pressed = false;
static bool s_long_sent = false;       /*!< 本次按下已触发长按 */
static int64_t s_last_click_ms = 0;    /*!< 上一次单击时刻 ms */
static bool s_pending_click = false;   /*!< 等待双击窗口的单击 */

static void input_task(void *arg);

esp_err_t srv_input_init(void)
{
    /* BOOT 键：上拉输入（按下为低） */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "gpio config failed");

    /* 记录初始电平，避免上电瞬间误判 */
    s_is_pressed = (gpio_get_level(BOOT_GPIO) == 0);

    if (xTaskCreate(input_task, "srv_input", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "BOOT key service started (GPIO%d)", BOOT_GPIO);
    return ESP_OK;
}

esp_err_t srv_input_register_cb(srv_input_cb_t cb, void *arg)
{
    s_cb = cb;
    s_cb_arg = arg;
    return ESP_OK;
}

static void fire_evt(srv_input_evt_t evt)
{
    if (s_cb) {
        s_cb(evt, s_cb_arg);
    } else {
        ESP_LOGI(TAG, "input evt: %d", (int)evt);
    }
}

static void input_task(void *arg)
{
    for (;;) {
        bool level = (gpio_get_level(BOOT_GPIO) == 0);

        if (level && !s_is_pressed) {
            /* 按下沿 */
            s_is_pressed = true;
            s_press_start = esp_timer_get_time();
            s_long_sent = false;
        } else if (!level && s_is_pressed) {
            /* 释放沿 */
            s_is_pressed = false;
            int64_t held_us = esp_timer_get_time() - s_press_start;

            if (held_us < LONG_PRESS_MS * 1000) {
                /* 单击候选：进入双击窗口 */
                int64_t now_ms = esp_timer_get_time() / 1000;
                if (s_pending_click && (now_ms - s_last_click_ms) <= DOUBLE_CLICK_WINDOW_MS) {
                    s_pending_click = false;
                    fire_evt(SRV_INPUT_EVT_DOUBLE_CLICK);
                } else {
                    s_pending_click = true;
                    s_last_click_ms = now_ms;
                }
            }
        }

        /* 长按检测（按下且未触发过） */
        if (s_is_pressed && !s_long_sent) {
            int64_t held_ms = (esp_timer_get_time() - s_press_start) / 1000;
            if (held_ms >= LONG_PRESS_MS) {
                s_long_sent = true;
                s_pending_click = false;
                fire_evt(SRV_INPUT_EVT_LONG_PRESS);
            }
        }

        /* 双击窗口超时，补发单击 */
        if (s_pending_click) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            if ((now_ms - s_last_click_ms) > DOUBLE_CLICK_WINDOW_MS) {
                s_pending_click = false;
                fire_evt(SRV_INPUT_EVT_SHORT_PRESS);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}
