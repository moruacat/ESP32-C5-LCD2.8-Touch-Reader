cmake_minimum_required(VERSION 3.16)

# 固件版本（语义化，见 README 的版本与发布一节）。
# 这个值会被 IDF 写进 esp_app_desc.version：关于页读它显示，
# 串口日志和烧录工具也能看到。
# 发版时只改这一行，其余地方不要硬编码版本号。
set(PROJECT_VER "1.5.2")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(clock_reader)
