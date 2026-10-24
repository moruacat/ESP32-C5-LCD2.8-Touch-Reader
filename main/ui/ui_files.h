#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建文件管理页面
 *
 * 浏览 SD 卡：点目录进去、点书直接读；长按弹菜单（打开 / 加入书架 /
 * 加入音乐库 / 删除）。底部显示 SD 卡占用情况。
 */
void ui_files_create(lv_obj_t *root);

#ifdef __cplusplus
}
#endif
