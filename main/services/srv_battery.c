#include "srv_battery.h"
#include "srv_config.h"
#include "esp_log.h"
#include "esp_err.h"
#include "bsp/esp32_c5_touch_lcd_2.8.h"
#include "custom_io_expander_ch32v003.h"

static const char *TAG = "srv_battery";

/*
 * 电池电压换算（CH32V003 的 ADC + 板上 3:1 分压）：
 *   ADC 10 位（0~1023）、参考电压 3.3V、分压比 3:1
 *   ADC 引脚电压 = adc / 1023 * 3.3
 *   电池电压     = ADC 引脚电压 * 3 = adc * 9.9 / 1023
 *
 * 注意分压比 3:1 意味着 ADC 满量程对应 9.9V，远高于锂电的 4.2V ——
 * 也就是 4.2V 时 ADC 才 434 左右，量程有一大半是浪费的。
 * 这是板上硬件定死的，换算时照实算就行，不要再补什么偏移量。
 */
#define BAT_ADC_FULL         (1023.0f)  /*!< 10 位 ADC 满量程 */
#define BAT_ADC_VREF         (3.3f)     /*!< ADC 参考电压 */
#define BAT_DIVIDER_RATIO    (3.0f)     /*!< 板上电池分压比 */

#define BAT_FULL_VOLTAGE     (4.2f)     /*!< 满电电压，仅用于「没装电池」时兜底显示 */

/*
 * 电压 → 电量必须走放电曲线，不能用线性映射。
 * 锂电中段是个平台：3.7~3.9V 之间就吃掉一大半电量，两头都很陡。
 * 线性算的话 3.4V 会报成 40% 出头，实际那会儿已经快关机了 ——
 * 用户会看着「还有 40%」突然黑屏。
 */
typedef struct {
    uint16_t mv;
    uint8_t  pct;
} bat_curve_pt_t;

/* 单节锂电的典型放电曲线（空载电压 → 剩余电量） */
static const bat_curve_pt_t s_curve[] = {
    {4200, 100}, {4100,  90}, {4000,  80}, {3930,  70}, {3870,  60},
    {3820,  50}, {3790,  40}, {3770,  30}, {3740,  20}, {3680,  10},
    {3500,   5}, {3300,   2}, {3000,   0},
};

#define BAT_CURVE_NUM   ((int)(sizeof(s_curve) / sizeof(s_curve[0])))

static int voltage_to_percent(float v)
{
    uint16_t mv = (uint16_t)(v * 1000.0f + 0.5f);

    if (mv >= s_curve[0].mv) {
        return 100;
    }
    for (int i = 0; i + 1 < BAT_CURVE_NUM; i++) {
        if (mv >= s_curve[i + 1].mv) {
            int hi = s_curve[i].mv;
            int lo = s_curve[i + 1].mv;
            int hi_pct = s_curve[i].pct;
            int lo_pct = s_curve[i + 1].pct;
            return lo_pct + (mv - lo) * (hi_pct - lo_pct) / (hi - lo);
        }
    }
    return 0;
}

/* 读数不做平滑：单次 ADC 值直接换算。
 * CH32 的 ADC 虽然有几个 LSB 抖动，但这里要的是「当前电压」的即时反映 ——
 * 平滑窗口和百分比迟滞会把变化拖慢，反而让人以为读数不准。 */

esp_err_t srv_battery_init(void)
{
    esp_io_expander_handle_t io = bsp_get_io_expander_handle();
    if (io == NULL) {
        ESP_LOGW(TAG, "IO expander not ready");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "battery monitor ready");
    return ESP_OK;
}

esp_err_t srv_battery_get_adc(uint16_t *adc)
{
    if (adc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_io_expander_handle_t io = bsp_get_io_expander_handle();
    if (io == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return custom_io_expander_get_adc(io, adc);
}

/*! ADC 原始值 → 未修正电压（按板上定死的 3:1 分压、10 位、3.3V 参考） */
static float adc_to_voltage(uint16_t adc)
{
    return (float)adc * BAT_ADC_VREF * BAT_DIVIDER_RATIO / BAT_ADC_FULL;
}

/*! 修正系数（千分比）：板上分压电阻有误差，单点校准过就用它折算 */
static int cal_k(void)
{
    return (int)srv_config_get_int(CFG_BAT_CAL_K, CFG_DEF_BAT_CAL_K);
}

esp_err_t srv_battery_get(float *voltage, int *percent)
{
    if (voltage == NULL || percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t adc = 0;
    esp_err_t ret = srv_battery_get_adc(&adc);
    if (ret != ESP_OK) {
        return ret;
    }

    /* ADC 读到 0 说明分压点没电压，通常是没装电池、只插着 USB。
     * 按满电显示，免得屏幕上顶个 0% 吓人。 */
    if (adc == 0) {
        *voltage = BAT_FULL_VOLTAGE;
        *percent = 100;
        return ESP_OK;
    }

    float v = adc_to_voltage(adc) * (float)cal_k() / 1000.0f;

    *voltage = v;
    *percent = voltage_to_percent(v);
    return ESP_OK;
}

int srv_battery_cal_k(void)
{
    return cal_k();
}

esp_err_t srv_battery_calibrate(float measured_v)
{
    if (measured_v < 2.0f || measured_v > 5.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t adc = 0;
    if (srv_battery_get_adc(&adc) != ESP_OK) {
        return ESP_FAIL;
    }
    if (adc == 0) {
        return ESP_ERR_INVALID_STATE;   /* 没装电池，没有可校准的读数 */
    }

    /* 单点校准：系数 = 实测电压 / 未修正电压（与当前系数无关，
     * 因为 adc_to_voltage 给的就是"未修正"的那个值） */
    const float v_raw = adc_to_voltage(adc);
    if (v_raw < 0.5f) {
        return ESP_ERR_INVALID_STATE;
    }

    int k = (int)(measured_v / v_raw * 1000.0f + 0.5f);
    if (k < 500)  k = 500;
    if (k > 2000) k = 2000;

    esp_err_t ret = srv_config_set_int(CFG_BAT_CAL_K, k, true);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "电池校准：实测 %.2fV / 未修正 %.2fV（ADC %u）→ 系数 %d‰",
                 (double)measured_v, (double)v_raw, (unsigned)adc, k);
    }
    return ret;
}

void srv_battery_cal_reset(void)
{
    (void)srv_config_set_int(CFG_BAT_CAL_K, CFG_DEF_BAT_CAL_K, true);
    ESP_LOGI(TAG, "电池校准已恢复默认（系数 1000‰）");
}
