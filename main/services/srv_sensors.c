#include "srv_sensors.h"
#include "srv_rtc.h"
#include "srv_sd.h"
#include "srv_fs.h"
#include "esp_log.h"
#include "esp_err.h"
#include "bsp/esp32_c5_touch_lcd_2.8.h"
#include "shtc3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>

static const char *TAG = "srv_sensors";

/* BSP 创建的 SHTC3 设备句柄（非 static 全局） */
extern i2c_master_dev_handle_t shtc3_handle;

#define RECORD_DIR_NAME     "temp-rec"
#define RECORD_DEFAULT_MIN  10

static TaskHandle_t s_rec_task = NULL;
static int          s_rec_interval_min = RECORD_DEFAULT_MIN;

esp_err_t srv_sensors_init(void)
{
    esp_err_t ret = qmi_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 init failed: %s", esp_err_to_name(ret));
    }

    /* 注意：PCF85063A RTC 由 srv_rtc_init() 管理。
     * 不要调用 BSP 的 pcf_init()，它每次开机都会强制写入固定时间。 */

    ret = shtc3_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SHTC3 init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

esp_err_t srv_sensors_get_temp_humi(float *temp_c, float *humi_pct)
{
    if (!temp_c || !humi_pct) {
        return ESP_ERR_INVALID_ARG;
    }
    if (shtc3_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 温度优先 + 时钟拉伸 + 正常模式 */
    return shtc3_get_th(shtc3_handle, SHTC3_REG_T_CSE_NM, temp_c, humi_pct);
}

/* ===================== 温湿度记录 ===================== */

/**
 * @brief 逐级创建目录（已存在视为成功）
 */
static void ensure_dir(const char *path)
{
    char tmp[SRV_FS_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);

    for (char *p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            srv_fs_mkdir(tmp);
            *p = '/';
        }
    }
    srv_fs_mkdir(tmp);
}

esp_err_t srv_sensors_record_path(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    srv_rtc_time_t t;
    if (srv_rtc_get_time(&t) != ESP_OK) {
        out[0] = '\0';
        return ESP_ERR_INVALID_STATE;
    }

    /* <SD>/temp-rec/YYYY/MM/DD.csv */
    snprintf(out, out_len, "%s/%s/%04d/%02d/%02d.csv",
             srv_sd_mount_point(), RECORD_DIR_NAME, t.year, t.month, t.day);
    return ESP_OK;
}

esp_err_t srv_sensors_record_now(void)
{
    if (!srv_sd_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 时间无效时不写，避免把数据记到错误日期的文件里 */
    srv_rtc_time_t t;
    if (srv_rtc_get_time(&t) != ESP_OK) {
        ESP_LOGW(TAG, "RTC 时间无效，跳过本次记录");
        return ESP_ERR_INVALID_STATE;
    }

    float temp = 0.0f, humi = 0.0f;
    esp_err_t ret = srv_sensors_get_temp_humi(&temp, &humi);
    if (ret != ESP_OK) {
        return ret;
    }

    char dir[SRV_FS_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/%s/%04d/%02d",
             srv_sd_mount_point(), RECORD_DIR_NAME, t.year, t.month);
    ensure_dir(dir);

    /* path = dir + "/" + DD + ".csv"，dir 最长可达 SRV_FS_PATH_MAX-1，故留出余量 */
    char path[SRV_FS_PATH_MAX + 24];
    snprintf(path, sizeof(path), "%s/%02d.csv", dir, t.day);

    bool need_header = !srv_fs_exists(path);

    FILE *fp = fopen(path, "a");
    if (fp == NULL) {
        ESP_LOGE(TAG, "无法写入 %s", path);
        return ESP_FAIL;
    }

    if (need_header) {
        /* 文件按天分，时间列不需要重复日期；%%RH 用于转义百分号 */
        fprintf(fp, "# 时间,温度(℃),湿度(%%RH)\n");
    }
    fprintf(fp, "%02d:%02d:%02d,%.1f,%.1f\n",
            t.hour, t.min, t.sec, temp, humi);
    fclose(fp);

    ESP_LOGI(TAG, "记录温湿度：%.1f℃ / %.1f%%RH -> %s", temp, humi, path);
    return ESP_OK;
}

static void record_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(s_rec_interval_min * 60 * 1000);

    /* 开机先立刻记一笔，便于确认通路 */
    srv_sensors_record_now();

    while (s_rec_task != NULL) {
        vTaskDelay(period);
        if (s_rec_task == NULL) {
            break;
        }
        srv_sensors_record_now();
    }
    vTaskDelete(NULL);
}

esp_err_t srv_sensors_record_start(int interval_min)
{
    if (s_rec_task != NULL) {
        return ESP_OK;
    }
    s_rec_interval_min = (interval_min > 0) ? interval_min : RECORD_DEFAULT_MIN;

    if (xTaskCreate(record_task, "th_record", 4096, NULL, 2, &s_rec_task) != pdPASS) {
        s_rec_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "温湿度记录已启动（每 %d 分钟，%s/temp-rec/YYYY/MM/DD.csv）",
             s_rec_interval_min, srv_sd_mount_point());
    return ESP_OK;
}

esp_err_t srv_sensors_record_stop(void)
{
    if (s_rec_task == NULL) {
        return ESP_OK;
    }
    TaskHandle_t t = s_rec_task;
    s_rec_task = NULL;
    vTaskDelay(pdMS_TO_TICKS(1500));      /* 等任务自己退出 */
    if (t != NULL) {
        vTaskDelete(t);
    }
    ESP_LOGI(TAG, "温湿度记录已停止");
    return ESP_OK;
}

bool srv_sensors_record_running(void)
{
    return s_rec_task != NULL;
}
