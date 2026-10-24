#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief BOOT 键输入事件
 */
typedef enum {
    SRV_INPUT_EVT_SHORT_PRESS,   /*!< 短按（<300ms） */
    SRV_INPUT_EVT_DOUBLE_CLICK,  /*!< 双击（500ms 内两次） */
    SRV_INPUT_EVT_LONG_PRESS,    /*!< 长按（≥1s） */
} srv_input_evt_t;

typedef void (*srv_input_cb_t)(srv_input_evt_t evt, void *arg);

/**
 * @brief 初始化 BOOT 键（GPIO28）轮询任务
 */
esp_err_t srv_input_init(void);

/**
 * @brief 注册按键事件回调（导航层使用）
 */
esp_err_t srv_input_register_cb(srv_input_cb_t cb, void *arg);

#ifdef __cplusplus
}
#endif
