#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 建 OTA 进度层（挂 lv_layer_top，默认隐藏）
 *
 * 局域网 OTA 推送时 srv_ota 会上报进度，这里 200ms 轮询一次并显示：
 * 进度条 + 百分比；推送完成后显示「更新完成，正在重启」，然后随设备重启消失。
 * 层是全屏可点击遮罩，更新期间挡住下层触摸，避免乱点触发页面切换。
 *
 * 必须在 LVGL 初始化之后调用（app_init 的 UI 初始化阶段）。
 */
void ui_ota_progress_init(void);

#ifdef __cplusplus
}
#endif
