#include "srv_sd.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp32_c5_touch_lcd_2.8.h"

static const char *TAG = "srv_sd";
static bool s_mounted = false;

esp_err_t srv_sd_init(void)
{
    esp_err_t ret = bsp_sdcard_mount();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed: %s", esp_err_to_name(ret));
        s_mounted = false;
        return ret;
    }
    s_mounted = true;
    ESP_LOGI(TAG, "SD card mounted at %s", BSP_SD_MOUNT_POINT);
    return ESP_OK;
}

bool srv_sd_is_mounted(void)
{
    return s_mounted;
}

esp_err_t srv_sd_unmount(void)
{
    if (!s_mounted) {
        return ESP_OK;
    }
    esp_err_t ret = bsp_sdcard_unmount();
    if (ret == ESP_OK) {
        s_mounted = false;
    }
    return ret;
}

const char *srv_sd_mount_point(void)
{
    return BSP_SD_MOUNT_POINT;
}

esp_err_t srv_sd_remount(void)
{
    srv_sd_unmount();
    vTaskDelay(pdMS_TO_TICKS(50));      /* 等卡内控制器复位 */
    return srv_sd_init();
}

esp_err_t srv_sd_get_info(uint32_t *total_kb, uint32_t *free_kb)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t total_bytes = 0, free_bytes = 0;
    esp_err_t ret = esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total_bytes, &free_bytes);
    if (ret != ESP_OK) {
        return ret;
    }

    if (total_kb) {
        *total_kb = (uint32_t)(total_bytes / 1024);
    }
    if (free_kb) {
        *free_kb = (uint32_t)(free_bytes / 1024);
    }
    return ESP_OK;
}
