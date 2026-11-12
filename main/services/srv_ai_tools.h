#pragma once
#include "esp_mcp_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 把小智平台能调用的设备工具注册到 MCP engine
 *
 * 提供（都是可逆、低风险的操作）：
 *   self.get_device_status            电量/WiFi/SD/时间/音量/亮度/播放状态
 *   self.audio_speaker.set_volume     音量 0-100
 *   self.screen.set_brightness        亮度 0-100
 *   self.device.set_time              设置时分
 *   self.device.show_message          在屏幕上弹一条提示
 */
void srv_ai_tools_register(esp_mcp_t *mcp);

#ifdef __cplusplus
}
#endif
