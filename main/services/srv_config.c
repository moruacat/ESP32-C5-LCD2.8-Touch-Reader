#include "srv_config.h"
#include "srv_sd.h"
#include "bsp/esp32_c5_touch_lcd_2.8.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

static const char *TAG = "srv_config";

/* 延时落盘：最后一次修改后 2 秒写盘，避免拖动亮度条时高频写 SD */
#define SAVE_DELAY_US   (2 * 1000 * 1000)
#define CFG_LINE_MAX    192

typedef enum {
    CFG_I32 = 0,
    CFG_STR,
} cfg_type_t;

typedef struct {
    const char *key;
    cfg_type_t  type;
    uint16_t    offset;
    uint16_t    len;        /* CFG_STR 专用：缓冲区大小 */
    int32_t     def;        /* CFG_I32 专用：默认值 */
    const char *def_str;    /* CFG_STR 专用：默认值 */
    int32_t     min;
    int32_t     max;
} cfg_field_t;

#define OFF(f)  ((uint16_t)offsetof(srv_config_t, f))

#define I32(k, f, d, lo, hi)                                                    \
    { .key = (k), .type = CFG_I32, .offset = OFF(f), .len = 0,                  \
      .def = (d), .def_str = NULL, .min = (lo), .max = (hi) }

#define IDX(k, f, i, d, lo, hi)                                                 \
    { .key = (k), .type = CFG_I32,                                              \
      .offset = (uint16_t)(OFF(f) + sizeof(int32_t) * (i)), .len = 0,           \
      .def = (d), .def_str = NULL, .min = (lo), .max = (hi) }

#define STR(k, f, d)                                                            \
    { .key = (k), .type = CFG_STR, .offset = OFF(f),                            \
      .len = (uint16_t)sizeof(((srv_config_t *)0)->f),                          \
      .def = 0, .def_str = (d), .min = 0, .max = 0 }

static const cfg_field_t s_fields[] = {
    /* 设备 */
    /* 亮度下限 1：0% 保留给休眠，不允许通过界面/配置调到 0 */
    I32(CFG_BRIGHTNESS,      brightness,       70,     1, 100),
    I32(CFG_VOLUME,          volume,           60,     0, 100),
    I32(CFG_DIM_EN,          dim_en,           CFG_DEF_DIM_EN,       0, 1),
    I32(CFG_DIM_AFTER,       dim_after,        CFG_DEF_DIM_AFTER,   10, 600),
    I32(CFG_LOCK_EN,         lock_en,          CFG_DEF_LOCK_EN,      0, 1),
    /* 锁屏比变暗晚一档：界面上的步进是 30 秒一格（30~3600 秒） */
    I32(CFG_LOCK_AFTER,      lock_after,       CFG_DEF_LOCK_AFTER,  30, 3600),
    /* 触摸唤醒：关掉后休眠中的点击不再亮屏，只能用 BOOT 键唤醒 */
    I32(CFG_TOUCH_WAKE,      touch_wake,       CFG_DEF_TOUCH_WAKE,   0, 1),
    I32(CFG_FAST_HOME,       fast_home,        CFG_DEF_FAST_HOME,    0, 1),
    /* 电池修正系数：0.5~2.0 倍，单点校准算出来落在这个区间里 */
    I32(CFG_BAT_CAL_K,       bat_cal_k,        CFG_DEF_BAT_CAL_K,    500, 2000),
    /* 安全模式：开关落盘，重启后保持上次的状态 */
    I32(CFG_SAFE_MODE,       safe_mode,        CFG_DEF_SAFE_MODE,    0, 1),

    /* 网络 */
    I32(CFG_WIFI_EN,         wifi_en,          0,      0, 1),
    STR(CFG_WIFI_SSID,       wifi_ssid,        ""),
    STR(CFG_WIFI_PASS,       wifi_pass,        ""),
    I32(CFG_NTP_EN,          ntp_en,           1,      0, 1),
    I32(CFG_NTP_TZ,          ntp_tz,           CFG_DEF_NTP_TZ,       -12, 14),
    I32(CFG_NTP_FINE,        ntp_fine,         CFG_DEF_NTP_FINE,     -60, 60),

    /* 闹钟 */
    I32(CFG_ALARM_VOLUME,    alarm_volume,     CFG_DEF_ALARM_VOLUME, 0, 100),
    IDX("alarm1_en",         alarm_en,  0,     0,      0, 1),
    IDX("alarm1_h",          alarm_h,   0,     7,      0, 23),
    IDX("alarm1_m",          alarm_m,   0,     0,      0, 59),
    IDX("alarm2_en",         alarm_en,  1,     0,      0, 1),
    IDX("alarm2_h",          alarm_h,   1,     12,     0, 23),
    IDX("alarm2_m",          alarm_m,   1,     0,      0, 59),
    IDX("alarm3_en",         alarm_en,  2,     0,      0, 1),
    IDX("alarm3_h",          alarm_h,   2,     18,     0, 23),
    IDX("alarm3_m",          alarm_m,   2,     0,      0, 59),

    /* 阅读 */
    I32(CFG_READ_FONT_SIZE,  read_font_size,     20,     8, 48),
    I32(CFG_READ_LETTER_SP,  read_letter_space, 0,     0, 10),
    I32(CFG_READ_LINE_SP,    read_line_space,  6,      0, 30),
    /* 段距一格 10：段间只能整行地留空，一格一档 */
    I32(CFG_READ_PARA_SP,    read_para_space,  0,      0, 40),
    /* 阅读器里默认允许自动变暗，想安安静静看书可以关掉 */
    I32(CFG_READ_KEEP_BRIGHT, read_keep_bright, 0,     0, 1),
    /* 书架按最近阅读排序：越后读的越靠前 */
    I32(CFG_SHELF_RECENT,    shelf_recent,      CFG_DEF_SHELF_RECENT, 0, 1),

    /* 传感器 */
    I32(CFG_TEMP_REC_EN,     temp_rec_en,      1,      0, 1),

    /* 游戏：重力感应方向校正（bit0 左右 / bit1 上下 / bit2 交换轴），默认不校正 */
    I32(CFG_MOTION_FLIP,     motion_flip,      0,      0, 7),

    /* 计时器 / 倒计时 */
    /* 到点提示音默认开；开关状态存在这里，界面上改一次就跟着落盘 */
    I32(CFG_TIMER_SOUND,     timer_sound,      1,      0, 1),

    /* 音乐 */
    IDX("mute1_en",          mute_en,   0,     0,      0, 1),
    IDX("mute1_s",           mute_s,    0,     480,    0, 1439),
    IDX("mute1_e",           mute_e,    0,     600,    0, 1439),
    IDX("mute2_en",          mute_en,   1,     0,      0, 1),
    IDX("mute2_s",           mute_s,    1,     720,    0, 1439),
    IDX("mute2_e",           mute_e,    1,     840,    0, 1439),
    IDX("mute3_en",          mute_en,   2,     0,      0, 1),
    IDX("mute3_s",           mute_s,    2,     960,    0, 1439),
    IDX("mute3_e",           mute_e,    2,     1080,   0, 1439),

    /* 照片：6 位数字密码，空串表示未设置（此时进照片不校验） */
    STR(CFG_PHOTO_PASS,      photo_pass,       ""),
};

#define FIELD_COUNT (sizeof(s_fields) / sizeof(s_fields[0]))

static srv_config_t       s_cfg;
static bool               s_dirty  = false;
static bool               s_loaded = false;
static esp_timer_handle_t s_save_timer;
static char               s_path[64];

/* ===================== 内部工具 ===================== */

static int32_t *as_i32(const cfg_field_t *f)
{
    return (int32_t *)(void *)((uint8_t *)&s_cfg + f->offset);
}

static char *as_str(const cfg_field_t *f)
{
    return (char *)(void *)((uint8_t *)&s_cfg + f->offset);
}

static const cfg_field_t *find_field(const char *key)
{
    for (size_t i = 0; i < FIELD_COUNT; i++) {
        if (strcmp(s_fields[i].key, key) == 0) {
            return &s_fields[i];
        }
    }
    return NULL;
}

/* 去掉首尾空白，返回新的起始指针 */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
    return s;
}

static void apply_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));

    for (size_t i = 0; i < FIELD_COUNT; i++) {
        const cfg_field_t *f = &s_fields[i];
        if (f->type == CFG_I32) {
            *as_i32(f) = f->def;
        } else {
            strncpy(as_str(f), f->def_str ? f->def_str : "", f->len - 1);
            as_str(f)[f->len - 1] = '\0';
        }
    }
}

/* 把一行 "key=value" 应用到配置；未知键返回 false */
static bool apply_line(char *line)
{
    char *p = trim(line);
    if (*p == '\0' || *p == '#') {
        return false;
    }

    char *eq = strchr(p, '=');
    if (eq == NULL) {
        return false;
    }
    *eq = '\0';

    const cfg_field_t *f = find_field(trim(p));
    if (f == NULL) {
        ESP_LOGD(TAG, "忽略未知配置项: %s", p);
        return false;
    }

    char *val = trim(eq + 1);

    if (f->type == CFG_I32) {
        long v = strtol(val, NULL, 10);
        if (v < f->min) v = f->min;
        if (v > f->max) v = f->max;
        *as_i32(f) = (int32_t)v;
    } else {
        strncpy(as_str(f), val, f->len - 1);
        as_str(f)[f->len - 1] = '\0';
    }
    return true;
}

/* ===================== 生命周期 ===================== */

static void save_timer_cb(void *arg)
{
    (void)arg;
    srv_config_save();
}

/* 从 /config.dat 载入；文件缺失或损坏时保持默认值并把 dirty 置位 */
static esp_err_t load_from_file(void)
{
    FILE *fp = fopen(s_path, "r");
    if (fp == NULL) {
        ESP_LOGW(TAG, "%s 不存在，使用默认值", s_path);
        s_dirty = true;
        return ESP_ERR_NOT_FOUND;
    }

    char line[CFG_LINE_MAX];
    int  applied = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (apply_line(line)) {
            applied++;
        }
    }
    fclose(fp);

    if (applied == 0) {
        ESP_LOGE(TAG, "%s 为空或已损坏，回退到默认值", s_path);
        apply_defaults();
        s_dirty = true;
        return ESP_ERR_INVALID_STATE;
    }

    if (applied < (int)FIELD_COUNT) {
        ESP_LOGW(TAG, "%s 仅 %d/%d 项有效，缺失项用默认值补齐",
                 s_path, applied, (int)FIELD_COUNT);
        s_dirty = true;
    }

    ESP_LOGI(TAG, "配置就绪（%d/%d 项来自文件）", applied, (int)FIELD_COUNT);
    return ESP_OK;
}

esp_err_t srv_config_init(void)
{
    snprintf(s_path, sizeof(s_path), "%s/config.dat", BSP_SD_MOUNT_POINT);

    if (s_save_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = save_timer_cb,
            .name     = "cfg_save",
        };
        esp_timer_create(&args, &s_save_timer);
    }

    apply_defaults();

    esp_err_t ret = ESP_OK;

    if (!srv_sd_is_mounted()) {
        ESP_LOGW(TAG, "SD 未挂载，配置仅驻留内存（挂载后需调用 srv_config_save）");
        s_dirty = true;
    } else {
        ret = load_from_file();
    }

    s_loaded = true;

    /* 首次启动或文件有缺失时写回一份完整配置，方便用户手工编辑 */
    if (s_dirty && srv_sd_is_mounted()) {
        srv_config_save();
    }

    srv_config_dump();
    return ret;
}

const srv_config_t *srv_config(void)
{
    return &s_cfg;
}

const char *srv_config_path(void)
{
    return s_path;
}

bool srv_config_is_dirty(void)
{
    return s_dirty;
}

/* ===================== 读写 ===================== */

esp_err_t srv_config_set_int(const char *key, int32_t value, bool save)
{
    const cfg_field_t *f = find_field(key);
    if (f == NULL || f->type != CFG_I32) {
        ESP_LOGW(TAG, "set_int: 无效键 %s", key ? key : "(null)");
        return ESP_ERR_NOT_FOUND;
    }

    if (value < f->min) value = f->min;
    if (value > f->max) value = f->max;

    if (*as_i32(f) == value) {
        return ESP_OK;      /* 值未变，不产生写入 */
    }

    *as_i32(f) = value;
    s_dirty = true;

    return save ? srv_config_save() : ESP_OK;
}

esp_err_t srv_config_set_str(const char *key, const char *value, bool save)
{
    const cfg_field_t *f = find_field(key);
    if (f == NULL || f->type != CFG_STR) {
        ESP_LOGW(TAG, "set_str: 无效键 %s", key ? key : "(null)");
        return ESP_ERR_NOT_FOUND;
    }

    char *dst = as_str(f);
    const char *src = value ? value : "";
    if (strncmp(dst, src, f->len) == 0) {
        return ESP_OK;
    }

    strncpy(dst, src, f->len - 1);
    dst[f->len - 1] = '\0';
    s_dirty = true;

    return save ? srv_config_save() : ESP_OK;
}

int32_t srv_config_get_int(const char *key, int32_t def_val)
{
    const cfg_field_t *f = find_field(key);
    if (f == NULL || f->type != CFG_I32) {
        return def_val;
    }
    return *as_i32(f);
}

/* ===================== 落盘 ===================== */

esp_err_t srv_config_save(void)
{
    if (!s_loaded) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!srv_sd_is_mounted()) {
        s_dirty = true;
        ESP_LOGW(TAG, "SD 未挂载，配置暂存内存");
        return ESP_ERR_INVALID_STATE;
    }

    FILE *fp = fopen(s_path, "w");
    if (fp == NULL) {
        s_dirty = true;
        ESP_LOGE(TAG, "无法写入 %s", s_path);
        return ESP_FAIL;
    }

    fprintf(fp, "# clock_reader config, 格式: key=value\n");

    for (size_t i = 0; i < FIELD_COUNT; i++) {
        const cfg_field_t *f = &s_fields[i];
        if (f->type == CFG_I32) {
            fprintf(fp, "%s=%ld\n", f->key, (long)*as_i32(f));
        } else {
            fprintf(fp, "%s=%s\n", f->key, as_str(f));
        }
    }

    fclose(fp);
    s_dirty = false;
    ESP_LOGI(TAG, "配置已保存到 %s", s_path);
    return ESP_OK;
}

void srv_config_save_deferred(void)
{
    s_dirty = true;
    if (s_save_timer == NULL) {
        return;
    }
    esp_timer_stop(s_save_timer);
    esp_timer_start_once(s_save_timer, SAVE_DELAY_US);
}

esp_err_t srv_config_reset(void)
{
    apply_defaults();
    s_dirty = true;
    ESP_LOGW(TAG, "配置已恢复默认值");
    return srv_config_save();
}

/* ===================== 调试输出 ===================== */

void srv_config_dump(void)
{
    printf("===== config (%s) =====\n", s_path);
    for (size_t i = 0; i < FIELD_COUNT; i++) {
        const cfg_field_t *f = &s_fields[i];
        if (f->type == CFG_I32) {
            printf("  %-20s= %ld\n", f->key, (long)*as_i32(f));
        } else {
            printf("  %-20s= \"%s\"\n", f->key, as_str(f));
        }
    }
    printf("===== config end =====\n");
}

esp_err_t srv_config_print(const char *key)
{
    const cfg_field_t *f = find_field(key);
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    if (f->type == CFG_I32) {
        printf("  %-20s= %ld\n", f->key, (long)*as_i32(f));
    } else {
        printf("  %-20s= \"%s\"\n", f->key, as_str(f));
    }
    return ESP_OK;
}
