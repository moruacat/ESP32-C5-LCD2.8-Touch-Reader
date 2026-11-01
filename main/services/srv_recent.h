#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 最近阅读记录：/books/.recent，每行 "<unix 时间戳>\t<绝对路径>"。
 *
 * 只保留最近 SRV_RECENT_MAX 本。书架排序开关打开时，srv_library_scan() 会按
 * 这里的时间把读过的书排到前面（越后读越靠前）。
 * 文件放在 /books 下，扩展名 .recent 不会被书架扫描当成书（扫描只认 .txt/.ebf）。
 */

#define SRV_RECENT_MAX 64

/** 加载记录文件（幂等；SD 未挂载时返回错误，不影响书架） */
esp_err_t srv_recent_init(void);

/** 记录一次打开：更新时间戳并写回文件 */
void srv_recent_touch(const char *path);

/** 上次打开时间（0 = 没读过） */
int64_t srv_recent_time(const char *path);

/** 记录文件路径（调试用） */
const char *srv_recent_path(void);

#ifdef __cplusplus
}
#endif
