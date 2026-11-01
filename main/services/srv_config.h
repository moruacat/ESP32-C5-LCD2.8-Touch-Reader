#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================== 容量定义 ===================== */
#define SRV_CONFIG_ALARM_NUM    3                       /*!< 闹钟数量 */
#define SRV_CONFIG_MUTE_NUM     3                       /*!< 静音时段数量 */

#define SRV_CONFIG_SSID_LEN     33                      /*!< WiFi SSID 缓冲（含结束符） */
#define SRV_CONFIG_WPASS_LEN    65                      /*!< WiFi 密码缓冲 */

/*! 照片密码：固定 6 位数字，缓冲多留一位结束符 */
#define SRV_CONFIG_PHOTO_PASS_LEN   7
#define SRV_CONFIG_PHOTO_PASS_DIGITS 6

/* ===================== 配置结构 ===================== */
/**
 * @brief 全部设备设置。对应新需求「设备文件存储」清单。
 *        字段为 0 时由 srv_config_init() 填入默认值，调用方只读。
 */
typedef struct {
    /* 设备 */
    int32_t brightness;                                 /*!< 背光亮度 0-100 */
    int32_t volume;                                     /*!< 全局音量 0-100 */
    int32_t dim_en;                                     /*!< 自动降低亮度开关 0/1 */
    int32_t dim_after;                                  /*!< 无操作多少秒后自动降低亮度 */
    int32_t lock_en;                                    /*!< 自动锁屏开关 0/1 */
    int32_t lock_after;                                 /*!< 无操作多少秒后自动锁屏（回时钟页并关屏） */
    int32_t touch_wake;                                 /*!< 触摸是否唤醒休眠中的屏幕 0/1 */
    int32_t fast_home;                                  /*!< 快速返回主页开关 0/1 */
    int32_t bat_cal_k;                                  /*!< 电池电压修正系数（千分比，1000 = 未校准） */
    int32_t safe_mode;                                  /*!< 安全模式开关 0/1（落盘，重启保留） */

    /* 网络 */
    int32_t wifi_en;                                    /*!< 网络开关 0/1 */
    char    wifi_ssid[SRV_CONFIG_SSID_LEN];             /*!< 当前网络名 */
    char    wifi_pass[SRV_CONFIG_WPASS_LEN];            /*!< 当前网络密码 */
    int32_t ntp_en;                                     /*!< NTP 开关 0/1 */
    int32_t ntp_tz;                                     /*!< 时区（小时，-12~+14） */
    int32_t ntp_fine;                                   /*!< 秒级微调偏移（-60~+60 秒） */

    /* 闹钟 */
    int32_t alarm_volume;                               /*!< 闹钟音量 0-100 */
    int32_t alarm_en[SRV_CONFIG_ALARM_NUM];             /*!< 闹钟开关 */
    int32_t alarm_h[SRV_CONFIG_ALARM_NUM];              /*!< 闹钟小时 0-23 */
    int32_t alarm_m[SRV_CONFIG_ALARM_NUM];              /*!< 闹钟分钟 0-59 */

    /* 阅读 */
    int32_t read_font_size;                             /*!< 字号 px（暂时没有多档字体，等做好再接） */
    int32_t read_letter_space;                          /*!< 字距 */
    int32_t read_line_space;                            /*!< 行距 */
    int32_t read_para_space;                            /*!< 段距：一格 10 = 段间一个空行 */
    int32_t read_keep_bright;                           /*!< 阅读器里不自动降亮度 0/1 */
    int32_t shelf_recent;                               /*!< 书架按最近阅读排序 0/1 */

    /* 传感器 */
    int32_t temp_rec_en;                                /*!< 温湿度记录开关 0/1 */

    /* 游戏 */
    int32_t motion_flip;                                /*!< 重力感应方向翻转：bit0 左右，bit1 上下 */

    /* 计时器 / 倒计时 */
    int32_t timer_sound;                                /*!< 倒计时到点提示音开关 0/1 */

    /* 音乐 */
    int32_t mute_en[SRV_CONFIG_MUTE_NUM];               /*!< 静音时段开关 */
    int32_t mute_s[SRV_CONFIG_MUTE_NUM];                /*!< 起始时刻（当天第几分钟） */
    int32_t mute_e[SRV_CONFIG_MUTE_NUM];                /*!< 结束时刻（当天第几分钟） */

    /* 照片 */
    char    photo_pass[SRV_CONFIG_PHOTO_PASS_LEN];      /*!< 照片密码（6 位数字，空串 = 未设置） */
} srv_config_t;

/* ===================== 字段键名（供 UI / 调试使用） ===================== */
#define CFG_BRIGHTNESS      "brightness"
#define CFG_VOLUME          "volume"
#define CFG_DIM_EN          "dim_en"
#define CFG_DIM_AFTER       "dim_after"
#define CFG_LOCK_EN         "lock_en"
#define CFG_LOCK_AFTER      "lock_after"
#define CFG_TOUCH_WAKE      "touch_wake"
#define CFG_FAST_HOME       "fast_home"
#define CFG_BAT_CAL_K       "bat_cal_k"
#define CFG_SAFE_MODE       "safe_mode"
#define CFG_WIFI_EN         "wifi_en"
#define CFG_WIFI_SSID       "wifi_ssid"
#define CFG_WIFI_PASS       "wifi_pass"
#define CFG_NTP_EN          "ntp_en"
#define CFG_NTP_TZ          "ntp_tz"
#define CFG_NTP_FINE        "ntp_fine"
#define CFG_ALARM_VOLUME    "alarm_volume"
#define CFG_READ_FONT_SIZE  "read_font_size"
#define CFG_READ_LETTER_SP  "read_letter_space"
#define CFG_READ_LINE_SP    "read_line_space"
#define CFG_READ_PARA_SP    "read_para_space"
#define CFG_READ_KEEP_BRIGHT "read_keep_bright"
#define CFG_SHELF_RECENT    "shelf_recent"
#define CFG_TEMP_REC_EN     "temp_rec_en"
#define CFG_TIMER_SOUND     "timer_sound"
#define CFG_MOTION_FLIP     "motion_flip"
#define CFG_PHOTO_PASS      "photo_pass"

/**
 * @brief 生成 alarmN_en / alarmN_h / alarmN_m 键（N=1..3）
 * @param idx 1..SRV_CONFIG_ALARM_NUM
 */
#define CFG_ALARM_KEY(suffix, idx)  ("alarm" #idx "_" suffix)

/**
 * @brief 生成 muteN_en / muteN_s / muteN_e 键（N=1..3）
 * @param idx 1..SRV_CONFIG_MUTE_NUM
 */
#define CFG_MUTE_KEY(suffix, idx)   ("mute" #idx "_" suffix)

/* ===================== 默认值（配置表与界面共用） ===================== */
/*! 自动降低亮度默认开 */
#define CFG_DEF_DIM_EN          1
/*! 默认闲置多久后自动降低亮度（秒） */
#define CFG_DEF_DIM_AFTER       30
/*! 自动锁屏默认开：闲置久了回时钟页并关屏，界面不会一直停在别处 */
#define CFG_DEF_LOCK_EN         1
/*! 默认闲置多久后自动锁屏（秒）；比变暗晚，先暗后锁 */
#define CFG_DEF_LOCK_AFTER      120
/*! 触摸默认唤醒休眠中的屏幕；关掉后只能用 BOOT 键唤醒 */
#define CFG_DEF_TOUCH_WAKE      1
/*! 书架默认按最近阅读排序（关掉按扫描顺序） */
#define CFG_DEF_SHELF_RECENT    1
/*! 「快速返回主页」默认关：先保持原来的逐级返回习惯 */
#define CFG_DEF_FAST_HOME       0
/*! 默认时区：UTC+8 */
#define CFG_DEF_NTP_TZ          8
/*! 默认秒级微调（秒） */
#define CFG_DEF_NTP_FINE        0
/*! 默认闹钟音量：与全局音量分开，免得半夜闹钟太响 */
#define CFG_DEF_ALARM_VOLUME    80
/*! 电池修正系数默认值（千分比）：1000 = 按板上 3:1 分压原样换算 */
#define CFG_DEF_BAT_CAL_K       1000
/*! 安全模式默认关：这个开关落盘，重启后保持上次的状态 */
#define CFG_DEF_SAFE_MODE       0

/* ===================== 生命周期 ===================== */
/**
 * @brief 初始化配置：填默认值后从 SD 卡的 /config.dat 加载。
 *        文件不存在或损坏时保持默认值并标记待写回，不阻塞启动。
 */
esp_err_t srv_config_init(void);

/**
 * @brief 只读访问当前配置
 */
const srv_config_t *srv_config(void);

/**
 * @brief 配置文件完整路径（SD 未挂载时仅用于日志展示）
 */
const char *srv_config_path(void);

/**
 * @brief 是否有未落盘的改动
 */
bool srv_config_is_dirty(void);

/* ===================== 读写 ===================== */
/**
 * @brief 按键名写入整型项（自动限幅）
 * @param key   见 CFG_* 宏
 * @param value 新值
 * @param save  true=立即落盘；false=仅改内存（配合 srv_config_save 或 _deferred）
 * @return ESP_OK / ESP_ERR_NOT_FOUND（键不存在）/ ESP_ERR_INVALID_ARG
 */
esp_err_t srv_config_set_int(const char *key, int32_t value, bool save);

/**
 * @brief 按键名写入字符串项
 */
esp_err_t srv_config_set_str(const char *key, const char *value, bool save);

/**
 * @brief 按键名读取整型项
 * @return 键不存在时返回 def_val
 */
int32_t srv_config_get_int(const char *key, int32_t def_val);

/* ===================== 落盘 ===================== */
/**
 * @brief 立即写回 /config.dat（SD 未挂载时返回错误并保持 dirty）
 */
esp_err_t srv_config_save(void);

/**
 * @brief 延时合并写回：连续调用只在最后一次后 2 秒真正落盘。
 *        适合亮度条拖动等高频修改。
 */
void srv_config_save_deferred(void);

/**
 * @brief 恢复全部默认值并立即落盘
 */
esp_err_t srv_config_reset(void);

/**
 * @brief 把当前配置以 key=value 形式打印到串口（与文件格式一致）
 */
void srv_config_dump(void);

/**
 * @brief 按键名以 key=value 形式打印单项
 * @return ESP_OK / ESP_ERR_NOT_FOUND
 */
esp_err_t srv_config_print(const char *key);

#ifdef __cplusplus
}
#endif
