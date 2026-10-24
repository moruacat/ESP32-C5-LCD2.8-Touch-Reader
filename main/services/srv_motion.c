#include "srv_motion.h"
#include "srv_config.h"
#include "bsp/esp32_c5_touch_lcd_2.8.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "srv_motion";

/* BSP 里的全局设备结构（定义处非 static），
 * 同 srv_sensors 里 extern shtc3_handle 的做法。 */
extern qmi8658_dev_t qmi_dev;

#define ONE_G_MPS2      9.807f
#define LPF_ALPHA       0.35f       /*!< 一阶低通：越大跟得越快、抖动越多 */
#define RESET_GAP_US    500000      /*!< 距上次读取超过 0.5s 视为新一轮，滤波重来 */

static bool    s_ready      = false;
static float   s_fx         = 0.0f;
static float   s_fy         = 0.0f;
static int64_t s_last_us    = 0;
static int     s_flip       = 0;    /*!< bit0 = 左右翻转，bit1 = 上下翻转 */

esp_err_t srv_motion_init(void)
{
    if (qmi_dev.dev_handle == NULL) {
        ESP_LOGW(TAG, "QMI8658 未就绪，重力感应不可用");
        return ESP_ERR_INVALID_STATE;
    }

    /* 方向校正开关读一次缓存住：读取在游戏循环里是每帧都调的，
     * 每次都去做键名查找没必要。改的时候走 set_flip 更新缓存。 */
    s_flip  = (int)srv_config_get_int(CFG_MOTION_FLIP, 0);
    s_ready = true;

    ESP_LOGI(TAG, "重力感应就绪（方向校正=0x%02X）", s_flip);
    return ESP_OK;
}

bool srv_motion_ready(void)
{
    return s_ready;
}

esp_err_t srv_motion_read_tilt(float *x, float *y)
{
    if (x == NULL || y == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    esp_err_t ret = qmi8658_read_accel(&qmi_dev, &ax, &ay, &az);
    if (ret != ESP_OK) {
        return ret;
    }

    /* BSP 里把加速度单位设成了 m/s²，这里统一换成 g（1.0 = 一个重力） */
    ax /= ONE_G_MPS2;
    ay /= ONE_G_MPS2;

    /* 基础映射：芯片 X 轴 → 屏幕横向，芯片 Y 轴 → 屏幕纵向。
     *
     * 这块板子上屏幕是横屏 320x240（面板原生 240x320 靠 swap_xy + mirror_x
     * 转过来），所以不能想当然地认为「芯片 Y 轴跟着面板长边」—— 实测下来
     * 就是 X→横、Y→纵这个最朴素的对应。
     *
     * 芯片实际的焊装朝向没法从代码推出来，所以三个开关兜底：
     *   bit2 交换两轴（差 90° 时用）
     *   bit0 左右翻转、bit1 上下翻转（差 180° 或轴正负反了时用）
     * 三个合起来能覆盖平面内全部 8 种正交朝向。 */
    float tx = ax;
    float ty = ay;

    if (s_flip & 0x04) {
        const float t = tx;
        tx = ty;
        ty = t;
    }
    if (s_flip & 0x01) {
        tx = -tx;
    }
    if (s_flip & 0x02) {
        ty = -ty;
    }

    const int64_t now = esp_timer_get_time();
    if (s_last_us == 0 || (now - s_last_us) > RESET_GAP_US) {
        /* 隔了很久（切页回来）就直接吃新值，不要从上一次的姿态慢慢挪过去 */
        s_fx = tx;
        s_fy = ty;
    } else {
        s_fx += (tx - s_fx) * LPF_ALPHA;
        s_fy += (ty - s_fy) * LPF_ALPHA;
    }
    s_last_us = now;

    *x = s_fx;
    *y = s_fy;
    return ESP_OK;
}

/* ===================== 方向翻转 ===================== */

bool srv_motion_swap_xy(void)
{
    return (s_flip & 0x04) != 0;
}

bool srv_motion_flip_x(void)
{
    return (s_flip & 0x01) != 0;
}

bool srv_motion_flip_y(void)
{
    return (s_flip & 0x02) != 0;
}

void srv_motion_set_flip(bool swap_xy, bool flip_x, bool flip_y)
{
    int v = 0;
    if (swap_xy) {
        v |= 0x04;
    }
    if (flip_x) {
        v |= 0x01;
    }
    if (flip_y) {
        v |= 0x02;
    }

    s_flip = v;
    srv_config_set_int(CFG_MOTION_FLIP, v, true);
}
