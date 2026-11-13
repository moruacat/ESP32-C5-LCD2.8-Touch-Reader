# ESP32-C5 伪装时钟阅读器

<p align="left">
  <img alt="ESP-IDF" src="https://img.shields.io/badge/ESP--IDF-v6.1-blue">
  <img alt="LVGL" src="https://img.shields.io/badge/LVGL-9.6-1585C0">
  <img alt="Target" src="https://img.shields.io/badge/target-ESP32--C5-2E8B57">
  <img alt="License" src="https://img.shields.io/badge/license-GPL--3.0-orange">
</p>

基于 **Waveshare ESP32-C5-Touch-LCD-2.8** 的便携阅读器 / 时钟项目。

设备默认以时钟伪装形态显示；进入主菜单后，可使用阅读器、离线词典、单词学习、音乐、电台、录音、照片、小智 AI、游戏等功能。项目使用 **ESP-IDF v6.1 + LVGL 9.6**，界面保持深色主题，主色为 `#1585C0`。

> 这是个人硬件项目，功能仍在持续完善。首次正式 Release 计划使用 `v1.0.0`，版本号以仓库最终发布的 tag 为准。

---

## 主要功能

### 时钟与系统

- 默认时钟伪装界面，支持时间、日期、温湿度、网络 / SD / NTP / 电量状态。
- 主菜单卡片式布局，进入后才显示阅读和其他功能入口。
- 自动降低亮度、自动锁屏、触摸唤醒、BOOT 键休眠 / 唤醒。
- 安全模式、隐藏菜单、快速返回主页、SD 卡热插拔。
- 电量监测、低电量提醒、温度湿度记录。

### 阅读器

- 支持 **TXT** 和项目自定义的 **EBF** 电子书格式。
- 左右点击 / 滑动翻页，章节跳转，TXT 页码跳转。
- 阅读长按菜单：封面、简介、跳转、自动翻页。
- 自动翻页间隔可输入秒数（5~600 秒），正文顶部细线显示进度。
- 断点续读、阅读字号 / 字距 / 行距 / 段距设置；阅读器跟随全局横竖屏方向，长按菜单里也能切换。
- 书架双列卡片、最近阅读排序、长按移出书架。
- 书架竖屏（270°）自动切换为单列卡片。

### 词典与单词学习

- 离线英汉词典，约 15 万词条。
- 中英双向查询、词形还原、变形查询、拼写建议。
- 词级直译、相关短语、考试徽章。
- 单词书单元学习、认识 / 不认识计分、错词本。
- 中文词库查询支持拼音、前缀匹配。
- 单词页跟随全局横竖屏方向（单词主页、变形查询和学习页共用），竖屏下输入键盘贴底显示。

> 当前未集成拼音输入法。界面输入以英文 / ASCII 为主，中文词库仍可通过拼音查询。

### 音乐、电台与录音

- 扫描 `/music`，支持播放列表、上一曲 / 下一曲 / 暂停、进度和音量。
- 音乐页跟随全局横竖屏方向；播放列表、选曲层和音量层会跟随布局。
- 中国之声 HLS 在线电台。
- ES8311 麦克风录音，保存为 WAV。
- 全局静音时段。
- 音乐 / 电台 / 录音 / AI 共用音频通道，切换前会先停止其他音源。

### 小智 AI

- 基于 `espressif/esp_xiaozhi`，支持 WebSocket / MQTT、MCP 工具和语音通道。
- 按键说话（PTT）：按住说话，松开结束；回答期间可再次按下打断 TTS。
- MCP 工具可查询设备状态、设置音量、亮度、时间，以及弹出屏幕提示。
- 首次使用需要在 xiaozhi.me 控制台输入 6 位激活码。
- 需要联网，不提供离线唤醒词。

### 照片与游戏

- `/photos` 下扫描 RGB565 `.raw` 照片，全屏左右滑动浏览。
- 支持照片密码和照片转换工具。
- 内置 2048、贪吃蛇、五子棋、画板、重力球、足球等小游戏。

### 时间与 NTP

- NTP 自动校时、时区设置、手动调时。
- NTP 秒级偏移，使用数值输入 + 加减按钮。
- 每日自动偏移：按天补偿 RTC 累计误差。
- 开机时根据上次日期补正关机期间漏掉的每日偏移；NTP 成功校时后重置基准。
- 闹钟、计时器、倒计时。
- 计时器 / 倒计时支持可选的 BOOT 键启停；开启时会在运行时强制触摸唤醒，但不写入配置。

---

## 硬件

| 项目 | 参数 |
|---|---|
| 开发板 | Waveshare ESP32-C5-Touch-LCD-2.8 |
| MCU | ESP32-C5，单核 RISC-V，240 MHz |
| 内存 | 8 MB PSRAM |
| Flash | 32 MB |
| 屏幕 | 2.8 英寸，320 × 240 横屏 |
| 存储 | microSD |
| 音频 | ES8311 |
| 温湿度 | SHTC3 |
| 姿态 | QMI8658 |
| RTC | PCF85063A |
| 电源 | 锂电池 + ADC 电量检测 |

> LCD 与 SD 卡共用 SPI2 总线。SD 大块读写可能阻塞 LCD 刷新，音视频 / 翻页卡顿时先排查总线竞争。

---

## 固件基线

- ESP-IDF：**v6.1**
- LVGL：**9.6.0**
- LVGL 适配层：`espressif/esp_lvgl_adapter` 0.4.x
- 工程名：`clock_reader`
- 分区：

```text
factory  6 MB
fonts    4 MB SPIFFS
storage  5 MB SPIFFS
```

固件编译产物：

```text
build/clock_reader.bin
```

---

## 编译与烧录

### 方式一：一键脚本（推荐）

在项目根目录执行：

```powershell
# 编译 + 完整烧录（默认 COM20）
.\build.ps1

# 只编译，不烧录
.\build.ps1 -NoFlash

# 只烧 app，日常改代码最快
.\build.ps1 -AppOnly

# 烧录完成后进入串口监控
.\build.ps1 -Monitor

# 指定串口
.\build.ps1 -Port COM7
```

> 脚本会导入 ESP-IDF 环境，并使用 `idf.py --no-ccache`。沙箱 / 杀毒软件环境下 ccache 可能卡死，不建议省略 `--no-ccache`。

### 方式二：手动 IDF 命令

```powershell
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
idf.py --no-ccache build
```

烧录 app：

```powershell
idf.py --no-ccache -p COM20 app-flash monitor
```

串口通常是 USB-Serial-JTAG，具体端口以设备管理器为准。

---

## CI 固件助手 GUI

项目内置一个 Windows GUI 工具，用于查看 GitHub Actions 构建、下载 CI 固件、烧录和推送 Git 提交。

位置：

```text
tools/ci_panel/
```

启动：

```powershell
python -m pip install -r tools\ci_panel\requirements.txt
python tools\ci_panel\main.py
```

也可以直接双击：

```text
tools\ci_panel\run.bat
```

功能：

- 查看最近 GitHub Actions 运行。
- 查看完整构建日志。
- 下载成功构建的 `firmware` artifact。
- 自动查找 ESP-IDF 自带的 `esptool.exe`。
- 选择 COM 口，一键烧录。
- 查看待推送 commit 状态，推送当前本地分支到 `origin`。

GitHub Token 只保存在本机，勾选「记住 Token」后使用 Windows DPAPI 加密，不写入仓库。

详细说明见 [`tools/ci_panel/README.md`](tools/ci_panel/README.md)。

---

## SD 卡目录

设备启动时从 SD 卡读取配置和素材。常用目录：

```text
SD/
├── config.dat              # 设备配置，自动生成
├── wifi.dat                # WiFi 历史，自动生成
├── books/                  # TXT / EBF 电子书
├── dict/                   # 离线词典数据
│   ├── dict.dat
│   ├── dict.idx
│   ├── variants.dat
│   ├── variants.idx
│   └── ...
├── music/                  # MP3 音乐
├── words/                  # 单词书单元
├── photos/                 # RGB565 .raw 照片
├── record/                 # 录音 WAV
└── temp-rec/               # 温湿度记录 YYYY/MM/DD.csv
```

> 大型原始词库、生成的二进制词典、照片原始素材和模拟器目录默认不纳入 Git。首次使用需要自行准备 SD 数据或运行 `tools/` 下的生成脚本。

---

## 素材与词典工具

| 工具 | 用途 |
|---|---|
| `tools/studio/` | EBF Studio：EPUB → EBF、图片 → RGB565 RAW 的 PySide6 GUI |
| `tools/epub2ebf/` | EPUB 转换命令行 / GUI |
| `tools/photo2raw/` | 图片转 RGB565 RAW |
| `tools/build_dict.py` | 构建离线英汉词典 |
| `tools/build_zh_dict.py` | 构建中英双向词典 |
| `tools/build_variants.py` | 构建词形 / 变形数据 |
| `tools/build_wordbook.py` | 生成单词书数据 |
| `tools/check_dict.py` | 检查词典格式、排序与索引 |
| `tools/dict_probe.py` | PC 端复刻设备查词算法 |
| `tools/gen_font.py` | 根据汉字表重新生成字体 |
| `tools/png2lvgl.py` | PNG 图标转换为 LVGL C 数组 |

示例：

```powershell
python tools/check_dict.py
python tools/dict_probe.py
python tools/gen_font.py ui_14
```

---

## PC 模拟器（本地开发资产）

项目开发时还维护了一套 PC 模拟器，共享 `main/ui` 和 `main/services` 的真实代码，仅替换显示、触摸、I2C、WiFi、音频等硬件层。

> `sim/` 目录默认不纳入 Git；如果你手头有模拟器目录，可按下面方式构建。

```powershell
cd sim

# 工程路径含 #，必须先显式 configure，再 build
& "C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" -S . -B build -G "MinGW Makefiles"
& "C:\Espressif\tools\cmake\4.0.3\bin\cmake.exe" --build build -j

.\build\clock_sim.exe
```

模拟器使用独立的 `sim/build/`，不会修改或烧录固件。详细说明见 `sim/README.md`。

---

## 目录结构

```text
main/
├── app/                    # 启动流程、页面导航、页面注册
│   ├── app_init.c
│   └── app_navigator.c
├── ui/                     # 纯 LVGL 界面
│   ├── ui_clock.c          # 时钟伪装页
│   ├── ui_home.c           # 主菜单
│   ├── ui_reader.c         # 阅读器
│   ├── ui_bookshelf.c
│   ├── ui_words.c          # 查词 / 翻译
│   ├── ui_word_study.c
│   ├── ui_music.c
│   ├── ui_radio.c
│   ├── ui_photos.c
│   ├── ui_ai.c
│   ├── ui_timer.c
│   ├── ui_countdown.c
│   ├── ui_settings.c
│   └── ui_set_*.c
├── services/               # 纯逻辑 / 驱动封装，不依赖 LVGL
│   ├── srv_config.c        # /config.dat
│   ├── srv_reader.c        # 阅读引擎
│   ├── srv_dict*.c         # 词典
│   ├── srv_player.c
│   ├── srv_radio.c
│   ├── srv_ntp.c
│   ├── srv_sensors.c
│   └── ...
└── assets/                 # 字体、图标、生成资源

components/                 # 本地 BSP 和板级组件
tools/                      # 固件构建、素材、词典、GUI 工具
sim/                        # PC 模拟器（本地，不纳入 Git）
```

代码分层约定：

- `main/ui/`：只做 LVGL 视图与交互，不直接访问 BSP 硬件。
- `main/services/`：业务逻辑与硬件封装，不直接操作 LVGL。
- `main/app/app_navigator.c`：所有页面必须在这里注册。
- 配置统一走 `srv_config`，不要直接读写 `/config.dat`。
- 页面定时器和静态控件指针需要在 `LV_EVENT_DELETE` 中正确清理。

---

## 已知限制

- 拼音输入法已移除，界面中文输入仍受限。
- 天气显示尚未实现。
- 横竖屏方向是**一个全局值**：除时钟、日历、游戏页（锁定横屏）外，所有页面共用，标题栏右端的小图标切换，方向持久化到 `config.dat` 的 `rot`。旧配置里的 `rot_bookshelf` / `rot_words` / `rot_music` 会自动迁移到 `rot`。竖屏下页面按 240x320 重新排版（列表行、卡片、键盘、弹层都跟随当前逻辑分辨率）。
- 运行时 PNG / JPEG 解码未启用，照片和封面优先使用预转换的 RGB565 RAW / C 数组。
- 小智 AI 依赖网络和激活码，没有离线唤醒词。
- 温湿度目前只有 CSV 记录，没有历史图表页。
- 真机长跑回归仍需覆盖：内存水位、SD 热插拔、音乐 + 翻页、AI 与音频通道切换。
- 新增功能前需要关注 app 分区余量（factory 6 MB）。

---

## 版本与发布

仓库目前没有历史 tag / Release。建议首个正式版本使用：

```text
v1.0.0
```

后续版本建议遵循语义化版本：

- Bug 修复：`v1.0.1`
- 新功能：`v1.1.0`
- 大改版或不兼容变更：`v2.0.0`

---

## 开发注意

- 不要自行删除或覆盖未提交的改动；改动前先看 `git status`。
- 固件构建产物、`managed_components/`、词库原始数据、照片素材、模拟器和构建缓存不提交。
- 修改共享 `main/ui` / `main/services` 后，优先在模拟器验证。
- 真机构建 / 烧录前确认串口和供电，烧录过程中不要断开设备。

---

## 许可证

本项目使用 **GNU General Public License v3.0**，详见 [`LICENSE`](LICENSE)。

第三方组件、字体、词典数据和图标资源遵循各自许可证；再分发前请确认对应素材的授权。
