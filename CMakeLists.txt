cmake_minimum_required(VERSION 3.16)

# 固件版本（语义化，见 README 的版本与发布一节）。
# 这个值会被 IDF 写进 esp_app_desc.version：
#   - 关于页读 esp_app_get_description()->version 显示；
#   - 局域网 OTA 的 GET /api/info 也返回它，CI 工具据此显示设备版本。
# 发版时只改这一行，其余地方不要硬编码版本号。
set(PROJECT_VER "1.5.1")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(clock_reader)
