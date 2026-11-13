#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 采样点数：60 秒一个点 × 90 个 ≈ 覆盖最近 1.5 小时（仅本次运行，不落盘） */
#define SRV_HIST_POINTS     90
/** 采样间隔（秒）；要和 SRV_HIST_POINTS 一起看，才能算出覆盖时长 */
#define SRV_HIST_INTERVAL_S 60

/**
 * @brief 一个采样点
 *
 * 用定点整数存，省 RAM：温度/湿度都乘 10，电量直接用百分比。
 * 读不到的值用约定哨兵表示（battery_pct = -1、temp_c10 = INT16_MIN），
 * 界面据此跳过而不是画成 0。
 */
typedef struct {
    uint32_t uptime_s;      /*!< 采样时刻（开机秒） */
    int16_t  battery_pct;   /*!< 电量百分比；-1 表示那一刻读不到 */
    uint16_t battery_mv;    /*!< 电池电压（毫伏） */
    int16_t  temp_c10;      /*!< 温度 ×10；INT16_MIN 表示读不到 */
    uint16_t humi_p10;      /*!< 湿度 ×10 */
} srv_hist_point_t;

/** @brief 启动采样任务（开机调用一次；首次采样立即执行，页面一打开就有点） */
esp_err_t srv_hist_init(void);

/** @brief 已有有效点数（<= SRV_HIST_POINTS） */
int srv_hist_count(void);

/** @brief 采样序号，每采一次 +1；界面用它判断要不要重画曲线 */
uint32_t srv_hist_seq(void);

/**
 * @brief 按时间顺序取采样点（最旧 → 最新）
 * @param out  输出数组
 * @param max  数组容量
 * @return 实际填入的点数
 */
int srv_hist_get(srv_hist_point_t *out, int max);

#ifdef __cplusplus
}
#endif
