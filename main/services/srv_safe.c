#include "srv_safe.h"

#include "srv_config.h"

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "safe";

/*! 连点次数 */
#define SAFE_TAPS           5
/*! 这几次点击必须落在多长的窗口里（3 秒，够从容地点 5 下） */
#define SAFE_TAP_WINDOW_US  (3 * 1000 * 1000)
/*! 点够之后等右滑的时间（项目.md：2 秒） */
#define SAFE_ARM_US         (2 * 1000 * 1000)

/* 最近几次点击的时刻（环形）。点够 5 次且都在窗口内 → 武装 */
static int64_t s_taps[SAFE_TAPS];
static int     s_tap_i = 0;
static int     s_tap_n = 0;

static int64_t s_armed_until_us = 0;

bool srv_safe_on(void)
{
    /* 开关就存在 /config.dat 里（重启保留），不另存内存副本：
     * 一份状态最省心 —— 「重置全部设置」这类改动也能立刻反映出来 */
    return srv_config_get_int(CFG_SAFE_MODE, CFG_DEF_SAFE_MODE) != 0;
}

bool srv_safe_toggle(void)
{
    const bool on = !srv_safe_on();

    if (srv_config_set_int(CFG_SAFE_MODE, on ? 1 : 0, true) != ESP_OK) {
        ESP_LOGW(TAG, "安全模式状态没落盘（SD 未挂载？），重启后会丢");
    }

    /* 手势状态一次用完就清干净，免得残留到下次 */
    s_armed_until_us = 0;
    s_tap_n = 0;

    ESP_LOGI(TAG, "安全模式 %s（已落盘，重启保留）",
             on ? "开启（主菜单功能卡 + 阅读 / 音乐 / 文件管理 / 照片密码 / 快速返回主页已收起）"
                : "关闭（全部入口恢复）");
    return on;
}

void srv_safe_tap(void)
{
    const int64_t now = esp_timer_get_time();

    s_taps[s_tap_i] = now;
    s_tap_i = (s_tap_i + 1) % SAFE_TAPS;
    if (s_tap_n < SAFE_TAPS) {
        s_tap_n++;
    }
    if (s_tap_n < SAFE_TAPS) {
        return;
    }

    /* 环里最旧的那次（也就是 5 次里的第 1 次）到现在还不到窗口长度，才算连点 */
    const int64_t oldest = s_taps[s_tap_i];
    if (now - oldest > SAFE_TAP_WINDOW_US) {
        return;
    }

    s_armed_until_us = now + SAFE_ARM_US;
    s_tap_n = 0;                        /* 重新数，免得一直武装着 */
    ESP_LOGI(TAG, "连点 %d 次 —— 2 秒内右滑确认", SAFE_TAPS);
}

bool srv_safe_armed(void)
{
    if (s_armed_until_us == 0) {
        return false;
    }
    if (esp_timer_get_time() > s_armed_until_us) {
        s_armed_until_us = 0;           /* 超时：这次手势作废 */
        s_tap_n = 0;
        ESP_LOGD(TAG, "等右滑超时");
        return false;
    }
    return true;
}

void srv_safe_disarm(void)
{
    s_armed_until_us = 0;
}