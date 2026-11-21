#include "srv_motion.h"
#include "srv_config.h"
#include "srv_wifi.h"
#include "bsp/esp32_c5_touch_lcd_2.8.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef ESP_PLATFORM
#include "lwip/inet.h"
#include "lwip/sockets.h"
#endif

#include <math.h>
#include <stdio.h>
#include <string.h>

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

/* ===================== 姿态融合 ===================== */

#define GRAV_G      9.807f
#define DEG2RAD     (3.14159265358979323846f / 180.0f)
#define KP_ACCEL    0.8f            /*!< 加速度计修正强度：越大收敛越快、晃动越明显 */
#define Q_INIT_GAP_US 3000000LL     /*!< 超过 3 秒没调，下一帧重新用加速度计初始化姿态 */

static float   s_qw = 1.0f, s_qx = 0.0f, s_qy = 0.0f, s_qz = 0.0f;
static int64_t s_sample_last_us = 0;
static uint32_t s_seq = 0;

static void quat_normalize(void)
{
    float n = sqrtf(s_qw * s_qw + s_qx * s_qx + s_qy * s_qy + s_qz * s_qz);
    if (n < 1e-6f) {
        s_qw = 1.0f;
        s_qx = s_qy = s_qz = 0.0f;
        return;
    }
    s_qw /= n;
    s_qx /= n;
    s_qy /= n;
    s_qz /= n;
}

/* 用重力方向初始化 roll / pitch（yaw 记 0），平放时接近单位四元数 */
static void quat_init_from_accel(float ax, float ay, float az)
{
    const float roll  = atan2f(ay, az);
    const float pitch = atan2f(-ax, sqrtf(ay * ay + az * az));
    const float cr = cosf(roll  * 0.5f), sr = sinf(roll  * 0.5f);
    const float cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);

    s_qw = cr * cp;
    s_qx = sr * cp;
    s_qy = cr * sp;
    s_qz = -sr * sp;        /* yaw = 0 */
    quat_normalize();
}

static void quat_rotate_gyro(float gx, float gy, float gz, float dt)
{
    /* q' = q + 0.5 * q ⊗ (0, ω) * dt */
    const float qw = s_qw, qx = s_qx, qy = s_qy, qz = s_qz;
    s_qw += 0.5f * (-qx * gx - qy * gy - qz * gz) * dt;
    s_qx += 0.5f * ( qw * gx + qy * gz - qz * gy) * dt;
    s_qy += 0.5f * ( qw * gy - qx * gz + qz * gx) * dt;
    s_qz += 0.5f * ( qw * gz + qx * gy - qy * gx) * dt;
    quat_normalize();
}

/* 用加速度计修正陀螺积分漂移（互补滤波） */
static void quat_correct_accel(float ax, float ay, float az, float dt)
{
    const float an = sqrtf(ax * ax + ay * ay + az * az);
    if (an < 0.4f || an > 2.0f) {
        return;             /* 剧烈运动/失重时不采信 */
    }
    const float axn = ax / an, ayn = ay / an, azn = az / an;

    /* 机体系里「估计的上方向」= 旋转矩阵的第三列 */
    const float vx = 2.0f * (s_qx * s_qz - s_qw * s_qy);
    const float vy = 2.0f * (s_qw * s_qx + s_qy * s_qz);
    const float vz = s_qw * s_qw - s_qx * s_qx - s_qy * s_qy + s_qz * s_qz;

    /* 误差 = 测量上方向 × 估计上方向 */
    const float ex = ayn * vz - azn * vy;
    const float ey = azn * vx - axn * vz;
    const float ez = axn * vy - ayn * vx;

    const float k = KP_ACCEL * dt;
    if (k <= 0.0f) {
        return;
    }
    /* 把修正量当作一个很小的旋转叠加到姿态上 */
    quat_rotate_gyro(ex * k, ey * k, ez * k, 1.0f);
}

esp_err_t srv_motion_read_sample(srv_motion_sample_t *out)
{
    if (out == NULL) {
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
    ax /= GRAV_G;
    ay /= GRAV_G;
    az /= GRAV_G;

    float gx = 0.0f, gy = 0.0f, gz = 0.0f;
    if (qmi8658_read_gyro(&qmi_dev, &gx, &gy, &gz) != ESP_OK) {
        gx = gy = gz = 0.0f;        /* 陀螺读失败就只靠加速度计，别让整帧失败 */
    }

    const int64_t now = esp_timer_get_time();
    float dt = 0.0f;
    if (s_sample_last_us == 0 || (now - s_sample_last_us) > Q_INIT_GAP_US) {
        quat_init_from_accel(ax, ay, az);
        s_sample_last_us = now;
    } else {
        dt = (float)(now - s_sample_last_us) / 1000000.0f;
        if (dt > 0.1f) {
            dt = 0.1f;
        }
        const float gxr = gx * DEG2RAD;
        const float gyr = gy * DEG2RAD;
        const float gzr = gz * DEG2RAD;
        quat_rotate_gyro(gxr, gyr, gzr, dt);
        quat_correct_accel(ax, ay, az, dt);
        s_sample_last_us = now;
    }

    out->seq = ++s_seq;
    out->ax = ax;  out->ay = ay;  out->az = az;
    out->gx = gx;  out->gy = gy;  out->gz = gz;
    out->qw = s_qw; out->qx = s_qx; out->qy = s_qy; out->qz = s_qz;
    return ESP_OK;
}

/* ===================== UDP 姿态广播 ===================== */

#ifdef ESP_PLATFORM

#define STREAM_PERIOD_MS    33      /*!< 约 30Hz */
#define STREAM_TASK_STACK   4096
#define STREAM_TASK_PRIO     3

static TaskHandle_t s_stream_task = NULL;
static volatile bool s_stream_run = false;

/**
 * @brief 广播一行文本（放大成整数，避免电脑端解析 locale 相关的小数点）
 *
 *   MOTION <seq> <t_ms> <qw> <qx> <qy> <qz> <ax> <ay> <az> <gx> <gy> <gz>
 *   四元数 ×10000，加速度 ×1000（g），角速度 ×100（°/s）
 */
static void stream_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "UDP socket 创建失败");
        s_stream_run = false;
        vTaskDelete(NULL);
        return;
    }

    int on = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(SRV_MOTION_UDP_PORT);
    dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    char line[192];
    bool net_ok = false;

    while (s_stream_run) {
        if (!srv_wifi_is_connected()) {
            if (net_ok) {
                ESP_LOGW(TAG, "网络断开，姿态广播暂停");
                net_ok = false;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!net_ok) {
            ESP_LOGI(TAG, "姿态广播开始：UDP %d 端口，约 30Hz", SRV_MOTION_UDP_PORT);
            net_ok = true;
        }

        srv_motion_sample_t s;
        if (srv_motion_read_sample(&s) == ESP_OK) {
            const int n = snprintf(line, sizeof(line),
                                   "MOTION %u %lld %d %d %d %d %d %d %d %d %d %d\n",
                                   (unsigned)s.seq,
                                   (long long)(esp_timer_get_time() / 1000),
                                   (int)(s.qw * 10000), (int)(s.qx * 10000),
                                   (int)(s.qy * 10000), (int)(s.qz * 10000),
                                   (int)(s.ax * 1000),  (int)(s.ay * 1000),
                                   (int)(s.az * 1000),
                                   (int)(s.gx * 100),   (int)(s.gy * 100),
                                   (int)(s.gz * 100));
            if (n > 0) {
                (void)sendto(sock, line, (size_t)n, 0,
                             (struct sockaddr *)&dst, sizeof(dst));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(STREAM_PERIOD_MS));
    }

    close(sock);
    s_stream_task = NULL;
    ESP_LOGI(TAG, "姿态广播已停止");
    vTaskDelete(NULL);
}

void srv_motion_stream_set_enabled(bool en)
{
    if (en) {
        if (s_stream_task != NULL) {
            return;
        }
        if (!s_ready) {
            ESP_LOGW(TAG, "传感器不可用，姿态广播未启动");
            return;
        }
        s_stream_run = true;
        if (xTaskCreate(stream_task, "motion_net", STREAM_TASK_STACK,
                        NULL, STREAM_TASK_PRIO, &s_stream_task) != pdPASS) {
            s_stream_run = false;
            s_stream_task = NULL;
            ESP_LOGE(TAG, "姿态广播任务创建失败");
        }
    } else {
        s_stream_run = false;
    }
}

bool srv_motion_stream_enabled(void)
{
    return s_stream_run || s_stream_task != NULL;
}

#else   /* 模拟器：没有 lwip/sockets，网络广播按「关」处理 */

void srv_motion_stream_set_enabled(bool en)
{
    (void)en;
}

bool srv_motion_stream_enabled(void)
{
    return false;
}

#endif  /* ESP_PLATFORM */

esp_err_t srv_motion_init(void)
{
    if (qmi_dev.dev_handle == NULL) {
        ESP_LOGW(TAG, "QMI8658 未就绪，重力感应不可用");
        return ESP_ERR_INVALID_STATE;
    }

    /* 姿态融合需要陀螺仪：BSP 的 qmi_init 只配了加速度计的量程/ODR，
     * 这里把陀螺仪显式打开并统一成 °/s（读出来的值直接就是 °/s）。 */
    qmi8658_set_gyro_unit_dps(&qmi_dev, true);
    qmi8658_set_gyro_range(&qmi_dev, QMI8658_GYRO_RANGE_512DPS);
    qmi8658_set_gyro_odr(&qmi_dev, QMI8658_GYRO_ODR_1000HZ);
    qmi8658_enable_sensors(&qmi_dev, QMI8658_ENABLE_ACCEL | QMI8658_ENABLE_GYRO);

    /* 方向校正开关读一次缓存住：读取在游戏循环里是每帧都调的，
     * 每次都去做键名查找没必要。改的时候走 set_flip 更新缓存。 */
    s_flip  = (int)srv_config_get_int(CFG_MOTION_FLIP, 0);
    s_ready = true;

    /* 姿态融合的初值先按当前重力方向对齐，避免上电后模型歪着 */
    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    if (qmi8658_read_accel(&qmi_dev, &ax, &ay, &az) == ESP_OK) {
        quat_init_from_accel(ax / GRAV_G, ay / GRAV_G, az / GRAV_G);
        s_sample_last_us = esp_timer_get_time();
    }

    ESP_LOGI(TAG, "重力感应就绪（方向校正=0x%02X，陀螺 512dps/1kHz）", s_flip);
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
