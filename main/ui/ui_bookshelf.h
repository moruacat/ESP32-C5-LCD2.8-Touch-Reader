#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 书架页：双列卡片列出 /books/ 里的书，点卡片进阅读器
 *
 * 首次进入若书架是空的会自动扫一遍 SD 卡。
 */
void ui_bookshelf_create(lv_obj_t *root);

#ifdef __cplusplus
}
#endif
