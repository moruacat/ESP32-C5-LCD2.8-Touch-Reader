#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 设置页的二级页
 *
 * 声明集中在这里而不是一页一对头文件：每个头文件只有一行声明，
 * 拆开反而不好找。实现仍然一页一个 .c。
 */
void ui_set_net_create(lv_obj_t *root);      /*!< 网络 */
void ui_set_net_add_create(lv_obj_t *root);  /*!< 网络->添加网络 */
void ui_set_time_create(lv_obj_t *root);     /*!< 时间 */
void ui_set_alarm_create(lv_obj_t *root);    /*!< 闹钟 */
void ui_set_read_create(lv_obj_t *root);     /*!< 阅读 */
void ui_set_music_create(lv_obj_t *root);    /*!< 音乐 */
void ui_set_bright_create(lv_obj_t *root);   /*!< 亮度 */
void ui_set_battery_create(lv_obj_t *root);  /*!< 电池 */
void ui_set_about_create(lv_obj_t *root);    /*!< 关于 */

#ifdef __cplusplus
}
#endif
