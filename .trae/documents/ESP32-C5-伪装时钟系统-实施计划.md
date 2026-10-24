# ESP32-C5 多功能伪装时钟娱乐系统 — 实施计划

## 一、背景（Context）

基于 [项目.md](file:///d:/#MORUA/danpianji/ESP32-C5-LCD2.8-Touch-Reader/项目.md)（功能需求 v1.2）与 [技术.txt](file:///d:/#MORUA/danpianji/ESP32-C5-LCD2.8-Touch-Reader/技术.txt)（ESP-IDF 6.1 / Waveshare ESP32-C5-Touch-LCD-2.8），开发一套以"桌面时钟"为伪装层的多功能娱乐系统：内置阅读器（TXT/EBF）、音乐播放器、音乐库、书架、文件管理、小游戏与工具，全深色主题，**以菜单与 BOOT 键为主导**。当前目录仅有两份需求文档，无任何代码。

用户已确认：**分阶段迭代**开发；音频解码采用 **ESP-GMF 新框架**。用户已备好资源：`font/HarmonyOS_Sans_SC_Medium.ttf`（8MB 中文字体）与 `icon/` 35 个 PNG 图标。

## 二、技术栈

| 层 | 技术 | 来源 |
|---|---|---|
| 框架 | ESP-IDF **v6.1.0**（目标 `esp32c5`，240MHz，384KB SRAM / 8MB PSRAM / 32MB Flash） | 本地已装 `D:/Dev-Kit/ESP-IDF/v6.1/esp-idf` |
| UI | **LVGL v9.x**（默认深色主题、触摸手势） | 组件管理器 `lvgl/lvgl: ^9` |
| LVGL 适配 | `espressif/esp_lvgl_adapter`（含 FreeType 字体、文件系统桥、PSRAM buffer） | 组件管理器（版本需实测，BSP 声明 0.4.*，优先用较新版本） |
| 板级 BSP | Waveshare 官方 `esp32_c5_touch_lcd_2_8`、`bsp_extra`、`waveshare__esp_lcd_touch_cst3530`、`pedrominatel__shtc3` | 复制自 GitHub `waveshareteam/ESP32-C5-Touch-LCD-2.8` 的 `example/ESP-IDF-V554/04_sd_music/components/` |
| IO 扩展 | `waveshare/custom_io_expander_ch32v003`（控制 TP_RST/LCD_RST/功放/背光PWM/电池ADC） | 组件管理器 |
| 传感器 | `waveshare/qmi8658`（六轴）、`waveshare/pcf85063a`（RTC）、`pedrominatel/shtc3`（温湿度） | 组件管理器 |
| 音频 Codec | `espressif/esp_codec_dev ~1.5`（ES8311，I2C+I2S） | 组件管理器（BSP 已依赖） |
| 音频解码 | **ESP-GMF** `espressif/gmf_audio`（AUDIO_DEC：MP3/AAC/FLAC/WAV/M4A），或走 Waveshare 已验证的 `esp_audio_simple_player` 封装 | 组件管理器 |
| 音乐库 | `espressif/esp_playlist ^0.5`（媒体目录+播放列表，兼容 v6.x） | 组件管理器 |
| 存储 | `esp_driver_sdspi` + `fatfs`（SPI 模式）；配置存 `/config.dat`、`/wifi.dat`（SD 卡） | ESP-IDF 内置 |
| 联网 | `esp_wifi`（2.4G/5G）+ `esp_netif` + `esp_http_client` + SNTP 校时 | ESP-IDF 内置 |

**硬件关键引脚**（来自官方 BSP，I2C 地址已验证无冲突：CH32=0x24 / QMI8658=0x6B / PCF85063A=0x51 / SHTC3=0x70 / ES8311=0x18 / CST3530=0x58）：
- LCD：SCK=6, MOSI=7, DC=9, CS=10（RST/背光 PWM 经 CH32）
- 触摸 CST3530：I2C(SDA=0/SCL=1), INT=5（RST 经 CH32）
- SD 卡：CLK=6, MOSI=7, MISO=8, CS=23（与 LCD 共用 SPI2，需互斥）
- I2S：SCLK=24, LRCK=25, DOUT=26, DIN=27；功放 PA 经 CH32
- **BOOT 键：GPIO28**

## 三、工程目录结构

```
ESP32-C5-LCD2.8-Touch-Reader/
├── CMakeLists.txt
├── sdkconfig.defaults (+ sdkconfig.defaults.esp32c5)
├── partitions.csv            # factory(6M) + fonts(8M) + storage(剩余)
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml     # lvgl/esp_lvgl_adapter/esp_playlist/gmf_audio 版本总锁
│   ├── app_main.c
│   ├── app/                  # 业务逻辑层（不碰驱动）
│   │   ├── app_init / app_navigator(页面栈+手势路由)
│   │   ├── app_clock / app_reader / app_bookshelf
│   │   ├── app_music / app_musiclib / app_filemgr
│   │   ├── app_settings / app_tools / app_games / app_hidden(安全模式)
│   ├── ui/                   # 纯 LVGL 视图层（只调 app_*）
│   │   ├── ui_common(深色主题/公共组件) / ui_clock / ui_menu
│   │   ├── ui_reader / ui_bookshelf / ui_music / ui_musiclib
│   │   ├── ui_filemgr / ui_settings / ui_tools / ui_games
│   ├── services/             # 系统服务（无 LVGL 依赖，包住 BSP）
│   │   ├── srv_sd / srv_config / srv_sensors(含摇晃检测)
│   │   ├── srv_rtc / srv_input(BOOT键+手势) / srv_battery
│   │   ├── srv_audio(音量/时间段静音) / srv_wifi / srv_ntp
│   └── assets/
│       ├── fonts/  icons/  assets_manifest.h
├── components/
│   ├── esp32_c5_touch_lcd_2_8/       # ★ 复制自 Waveshare，需改 idf_component.yml
│   ├── waveshare__esp_lcd_touch_cst3530/
│   ├── pedrominatel__shtc3/
│   ├── ebf/                          # 自研：EBF 解析（chapter.N.ebfl/data.ebfl/cover.raw/progress.dat）
│   ├── txt_reader/                   # 自研：编码检测(UTF-8/GBK/UTF-16)、分块定位、PSRAM 块缓存
│   └── ui_kit/                       # 自研：通用列表/弹窗/滑块/进度条/状态栏
└── tools/
    └── epub2ebf/                     # PC 端 Python 转换工具（不进固件）
```

**模块归属**：UI/业务/服务放 `main/`；纯解析逻辑（ebf、txt_reader）、跨页复用（ui_kit）独立 component；BSP 直接复制 Waveshare 官方组件不做二次封装，由 `services/` 层包住，UI 不直接 include BSP 头。

## 四、每个位置的菜单与 BOOT 键功能矩阵（核心交付）

> 全局 BOOT 键（GPIO28）统一行为，所有界面生效，优先级：长按 > 双击 > 短按。

| BOOT 键操作 | 触发条件 | 全局行为 |
|---|---|---|
| **长按** | 按住 ≥1s | **快速隐藏**：立即返回时钟界面（所有位置最高优先级） |
| **双击** | 500ms 内两次 | 安全模式开启时 → 打开"阅读器/音乐"选择菜单；未开启 → 同短按 |
| **短按** | <300ms | 按当前位置上下文执行（见各位置） |

| 位置 | 菜单（触摸/长按） | BOOT 短按 |
|---|---|---|
| **① 时钟界面**（伪装层） | 从右往左滑 → 打开主菜单；长按屏幕任意处 → 无操作（保持伪装） | 打开主菜单 |
| **② 主菜单**（图标网格） | 点击图标进入对应功能；项目包含：阅读器、音乐、音乐库、书架、文件管理、设置、计算器、日历、计时器、游戏；**底部亮度条**：拖动实时调节背光亮度（经 CH32 PWM）并持久化到 `/config.dat` | 返回时钟界面 |
| **③ 阅读器** | 左右滑动翻页、上下滑动滚动；**长按 → 阅读菜单**：退出阅读 / 书籍详情 / 查看封面 / 字号± / 行距± / 音量± / 深浅色背景切换 / 隐藏状态栏(沉浸式) | 返回书架（阅读进度自动保存） |
| **④ 书架** | 网格/列表视图切换；点击书 → 打开阅读；**长按书 → 菜单**：查看封面 / 查看简介 / 移除外部书籍 / 排序(最近阅读/书名/添加时间) | 返回主菜单 |
| **⑤ 音乐播放器** | 播放/暂停、上一曲/下一曲、进度条拖动；**菜单键 → 循环模式**(顺序/单曲/列表/随机)、添加到播放列表、音量 | 返回音乐库（或进入前的界面） |
| **⑥ 音乐库** | 点击曲目 → 直接播放；**长按曲目 → 菜单**：加入播放列表 / 查看详情 / 删除记录(仅外部)；视图切换、格式筛选(MP3/WAV/AAC)、按文件名排序、搜索 | 返回主菜单 |
| **⑦ 文件管理器** | 点击进入目录；**菜单键 → 菜单**：添加外部书籍到书架 / 添加外部音乐到媒体库 / 删除文件/文件夹(确认提示) / 重新检测 SD 卡 / 弹出 SD 卡；返回按钮回上一级 | 返回上一级目录；根目录时返回主菜单 |
| **⑧ 设置** | 子菜单列表：设备(亮度/音量/SD/电池)、时间与校时(NTP/时区偏移/显示秒数)、主题(深浅)、音乐(时间段自动静音开关/静音时间段/扫描深度/格式过滤)、关于 | 返回主菜单 |
| **⑨ 关于界面** | 设备信息、固件版本；**连续点击"开发者"5 次 → 切换安全模式**（激活后摇晃两次进入"阅读器/音乐"选择菜单） | 返回设置 |
| **⑩ 安全模式选择菜单** | "阅读器 / 音乐"二选一；防误触：1s 内两次摇晃且由静止突变才响应 | 返回时钟界面 |
| **⑪ 计算器** | 数字键盘直接操作 | 返回主菜单 |
| **⑫ 日历** | 月历显示、翻月 | 返回主菜单 |
| **⑬ 计时器/倒计时** | 开始/暂停/重置；倒计时到点提醒 | 返回主菜单 |
| **⑭ 游戏** | 触摸操作；**菜单键 → 菜单**：重新开始 / 退出游戏；2048 参考 Waveshare 官方 `02_2048` 示例 | 返回游戏列表 |
| **⑮ 工具选择/游戏列表** | 点击进入各工具 | 返回主菜单 |

## 五、分阶段实施（每阶段可编译可烧录）

1. **阶段 1 — 工程骨架 + BSP + LVGL 点亮时钟**
   复制 Waveshare 组件、改 BSP `idf_component.yml`（R1 兼容桥头堡）；建 CMake/分区/配置；`app_main.c` 按序初始化（CH32→SD→传感器→音频→LVGL）；深色时钟页（大时:分+秒、日期星期、温湿度、状态栏占位）；触摸响应；BOOT 键消抖。**验证**：烧录走秒、触摸坐标正确、6 个 I2C 设备 probe 成功。
2. **阶段 2 — 导航框架 + 主菜单 + 设置骨架**
   `app_navigator`（页面栈+左滑手势）；`ui_menu` 图标网格（接入图标方案）；**主菜单底部亮度条**（`ui_kit` 滑块 + CH32 背光 PWM 实时调节 + `/config.dat` 持久化）；`srv_config`（/config.dat 持久化）；BOOT 长按一键回时钟。**验证**：任意页可左滑回时钟、菜单亮度条拖动实时生效且重启保留、音量重启生效。
3. **阶段 3 — 文件系统 + 书架 + 阅读器（工作量最大）**
   `srv_sd`（挂载/热插拔）、文件管理器、`/books` 扫描 + others.dat、书架（封面/排序）、TXT 阅读（编码检测、分块 2-4KB、断点续读）、EBF 阅读（ebf 组件）、字号/行距/背景、沉浸式。**验证**：中文 TXT 生僻字不丢、进度可续、翻页流畅。
4. **阶段 4 — 音频栈 + 音乐库 + 播放器**
   `gmf_audio`/`esp_audio_simple_player` + `esp_codec_dev` 管线；`esp_playlist` 扫描 `/music` + `.media_db`；播放器 UI（进度拖动/循环/音量）；时间段自动静音。**验证**：MP3/WAV 出声、重启后媒体库保留、到点静音。
5. **阶段 5 — 工具 + 游戏 + 安全模式 + 联网**
   计算器/日历/计时器；2048 等小游戏；安全模式（5 击开发者 + 摇晃两次选择菜单）；WiFi（/wifi.dat 记忆）+ NTP + 天气；低电量红闪；自动锁屏。**验证**：摇晃触发正确、NTP 校时准确。
6. **阶段 6 — 性能/内存打磨**
   heap 水位画像、LVGL buffer 调优、SD 与 LVGL flush 互斥验证、长跑无泄漏。

## 六、字体与图标方案

- **字体（混合方案）**：
  - 静态 UI 字库：`lv_font_conv` 预转换时钟大数字（48px 仅 0-9/:）与菜单常用字（~500 汉字 @14px）→ C 数组，保证启动与滚动速度；
  - 动态文本：**LVGL FreeType** 运行时渲染 HarmonyOS TTF（`LV_USE_FREETYPE` + adapter 的 `ESP_LV_ADAPTER_ENABLE_FREETYPE`），阅读器正文/列表等不可预测文本任意字号不丢字；glyph cache 放 PSRAM；
  - TTF 放 `fonts` 分区（spiffs/esp_mmap_assets）。
- **图标**：35 个 PNG 用 `lv_img_conv` 转为 C 数组（RGB565+A8），约 200KB 嵌入 flash，零运行时解码，接入点收敛于 `assets_manifest.h`。

## 七、关键风险与规避

- **R1（最高）esp_lcd v6 API 重构 vs Waveshare v5.5.4 BSP**：`esp_lcd_new_panel_io_spi()` 签名已变。规避：复制 BSP 本地化并改依赖版本，阶段 1 先做"最小可编译"冒烟，按编译错误逐个适配，`esp_lcd_panel_io_additions.h` 报错则注释（BSP 未实际调用）。
- **R2 esp_playlist/gmf_audio 与 v6.1 兼容**：esp_playlist 0.5.x 官方声明支持 v5.5/v6.0；GMF 优先走 Waveshare 已验证的 `esp_audio_simple_player + esp_codec_dev` 路径，预留降级（先 MP3/WAV）。
- **R3 初始化顺序**：严格按 Waveshare `main.c`：LCD CS 拉高 → CH32 IO 扩展 → 延时 100ms → SD → 传感器 → 音频。
- **R4 LCD 与 SD 共用 SPI2**：LVGL flush 与 SD 读写并发错乱 → 互斥信号量或错峰。
- **R5 内存**：LVGL buffer/章节缓存/FreeType 缓存/解码 ringbuffer 一律 `MALLOC_CAP_SPIRAM`；`SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`。
- **R6 FATFS 中文长文件名**：`CONFIG_FATFS_LONG_FILENAMES=y`。

## 八、验证方式

每阶段结束执行 `idf.py build flash monitor`（串口 COM 需确认），按各阶段验证标准检查；最终整机回归：时钟伪装 → 左滑主菜单 → 阅读/音乐/文件/设置全链路 → BOOT 长按快速隐藏 → 安全模式摇晃切换 → 拔插 SD → 长跑内存监控。

**关键文件**：`main/idf_component.yml`（依赖总锁）、`components/esp32_c5_touch_lcd_2_8/idf_component.yml`（BSP 兼容桥头堡）、`sdkconfig.defaults`、`partitions.csv`、`main/app/app_init.c`（硬件初始化顺序）、`main/ui/ui_menu.c`（主菜单网格）、`main/services/srv_input.c`（BOOT 键/手势分发，功能矩阵落地）。
