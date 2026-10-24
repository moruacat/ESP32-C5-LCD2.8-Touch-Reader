#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 挂载 SD 卡（失败不阻塞启动）
 */
esp_err_t srv_sd_init(void);

/**
 * @brief SD 卡是否已挂载
 */
bool srv_sd_is_mounted(void);

/**
 * @brief 卸载 SD 卡
 */
esp_err_t srv_sd_unmount(void);

/**
 * @brief 重新挂载 SD 卡（卸载后再挂载，对应「重载 SD 卡」）
 *        本板 SD 卡无 CD（插拔检测）引脚，热插拔只能靠本函数探测
 */
esp_err_t srv_sd_remount(void);

/**
 * @brief SD 卡挂载点（如 "/sdcard"）
 */
const char *srv_sd_mount_point(void);

/**
 * @brief 查询 SD 卡容量
 * @param total_kb 总容量（KB），可为 NULL
 * @param free_kb  剩余容量（KB），可为 NULL
 */
esp_err_t srv_sd_get_info(uint32_t *total_kb, uint32_t *free_kb);

#ifdef __cplusplus
}
#endif
