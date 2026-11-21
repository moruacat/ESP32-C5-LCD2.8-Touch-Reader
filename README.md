# ESP32-C5 伪装时钟阅读器

<p align="left">
  <img alt="ESP-IDF" src="https://img.shields.io/badge/ESP--IDF-v6.1-blue">
  <img alt="LVGL" src="https://img.shields.io/badge/LVGL-9.6-1585C0">
  <img alt="Target" src="https://img.shields.io/badge/target-ESP32--C5-2E8B57">
  <img alt="License" src="https://img.shields.io/badge/license-GPL--3.0-orange">
</p>

基于 **Waveshare ESP32-C5-Touch-LCD-2.8** 的便携阅读器 / 时钟项目。

设备默认以时钟伪装形态显示；进入主菜单后，可使用阅读器、离线词典、单词学习、音乐、电台、录音、照片、小智 AI、游戏等功能。项目使用 **ESP-IDF v6.1 + LVGL 9.6**，界面保持深色主题，主色为 `#1585C0`。

---

## 主要功能

### 时钟与系统

- 默认时钟伪装界面，支持时间、日期、农历、温湿度、网络 / SD / NTP / 电量状态。
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
- 字号支持 **14 / 16 / 18 / 20 / 22px**：14px 内嵌在固件里，其余档位由 `tools/gen_read_fonts.py` 生成 `.bin` 字库，默认打进 Flash 的 `font` 分区（无 SD 也能用）；SD 卡 `/fonts` 里有同名文件时优先用 SD 的。「设置 → 阅读 → 字号」循环切换，没有字库的档位会自动跳过。
- 段距按「格」调节（1 格 = 段间 1 个空行），所有可选值都真实生效。
- 书架双列卡片、最近阅读排序、长按移出书架。
- 书架竖屏（270°）自动切换为单列卡片。
- 转屏不会重扫书架、不会清空单词搜索、不会重连小智 AI。

### 词典与单词学习

- 离线英汉词典，SD 卡完整版约 15.7 万词条。
- 中英双向查询、词形还原、变形查询、拼写建议。
- 词级直译、相关短语、考试徽章。
- 单词书单元学习、认识 / 不认识计分、错词本。
- 中文词库查询支持拼音、前缀匹配。
- 词库只放 SD 卡：完整离线英汉词典约 27 万条（`/dict`），Flash 空间留给阅读字库。
- 单词页跟随全局横竖屏方向（单词主页、变形查询和学习页共用），竖屏下输入键盘贴底显示。

> 当前未集成拼音输入法。界面输入以英文 / ASCII 为主，中文词库仍可通过拼音查询。

### 音乐、电台与录音

- 扫描 `/music`，支持播放列表、上一曲 / 下一曲 / 暂停、进度和音量。
- WAV（包括录音机录出来的 16kHz 单声道）按裸 PCM 直通播放；此前 WAV 会被当成「没有文件头」而解码失败。
- 录音机页列出 `/record/rec-N.wav`，点一条就能试听/停止，不用离开页面。
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

### 姿态广播（电脑端 3D）

- 在「设置 → 关于 → 姿态广播」打开后，设备通过 UDP 4210 端口约 30Hz 广播四元数、加速度和角速度。
- 电脑端运行 `tools/motion3d/motion_view.py`（PyQt5 + numpy），实时显示外壳模型跟随设备旋转（只做姿态，不做加速度积分的位移）。
- 默认是简化外壳；用 `tools/motion3d/convert_step.py` 可以把 STEP 外壳转成 `model.obj` 后自动加载（面数上限 2 万，超了会拒绝，避免软件渲染卡死）。
- 详见 [`tools/motion3d/README.md`](tools/motion3d/README.md)。

### 时间与 NTP

- NTP 自动校时、时区设置、手动调时。
- NTP 秒级偏移，使用数值输入 + 加减按钮。
- 每日自动偏移：按天补偿 RTC 累计误差。
- 开机时根据上次日期补正关机期间漏掉的每日偏移；NTP 成功校时后重置基准。
- 闹钟、计时器、倒计时。
- 计时器 / 倒计时支持可选的 BOOT 键启停；开启时会在运行时强制触摸唤醒（防止BUG导致无法亮屏），但不写入配置。

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
- 当前版本：**1.5.4**（根 `CMakeLists.txt` 的 `PROJECT_VER`）
- 分区：

```text
factory  6 MB
ota_0    6 MB
ota_1    6 MB
font     6 MB SPIFFS（16/18/20/22px 阅读字库，2bpp，构建时生成）
```

> 设置不再存 SD 卡：整份配置打包写进 Flash 的 `nvs` 分区（带 magic / 版本 / 长度 / CRC32 校验），
> 读坏了自动回默认值，不会因为配置文件损坏而反复重启。旧的 `/config.dat` 只在首次启动时导入一次。

固件编译产物：

```text
build/clock_reader.bin
build/font.bin          # flash_assets/fonts 存在时才生成
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
>
> **1.5.4 改了分区表**（新增 6MB 的 `font` 字库分区），第一次必须完整烧录一次；
> 之后只烧 app（`-AppOnly`）即可。OTA 也不会更新分区表。

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

### CI（GitHub Actions）

`.github/workflows/build.yml` 在 push / PR 时用 `espressif/idf:v6.1` 容器跑一次真机编译
（`idf.py reconfigure` + `idf.py build`）。改完代码推送后看 Actions 结果即可，不需要本地
装 IDF 也能验证编译。

CI 构建没有 `flash_assets/`，artifact 里不含阅读字库镜像。字库在本地一键生成：

```powershell
python tools/gen_read_fonts.py
```

它会生成 `font/font_ui_{16,18,20,22}.bin`、`flash_assets/fonts/` 与
`build/font.bin`（6MB SPIFFS 镜像）。用 `tools/ci_panel` 烧录 CI 固件时，面板会自动
检测并追加 `build/font.bin` 到 `0x1220000`；没有这个文件时只烧固件，字号回退 SD
`/fonts` 或内嵌的 14px。词库仍然只放 SD 卡，不参与烧录。

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

设备启动时从 SD 卡读取书籍、词库和素材。设置已经改存 Flash（NVS），不再依赖 SD 卡。常用目录：

```text
SD/
├── config.dat              # 旧版设置（只在本版第一次启动时导入 NVS，之后不再写）
├── wifi.dat                # WiFi 历史，自动生成
├── books/                  # TXT / EBF 电子书
├── dict/                   # 完整离线词典（查词唯一来源）
│   ├── dict.dat
│   ├── dict.idx
│   ├── variants.dat
│   ├── variants.idx
│   └── ...
├── fonts/                  # 阅读字库（可选；有则优先于 Flash 的 font 分区）
│   ├── font_ui_16.bin
│   ├── font_ui_18.bin
│   ├── font_ui_20.bin
│   └── font_ui_22.bin
├── music/                  # MP3 / WAV 音乐
├── words/                  # 单词书单元
├── photos/                 # RGB565 .raw 照片
├── record/                 # 录音 WAV（录音机页可直接回放）
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
| `tools/build_flash_dict.py` | 可选：从完整词库裁一份精简词库（备份/小卡用，不再打进 Flash） |
| `tools/build_zh_dict.py` | 构建中英双向词典 |
| `tools/build_variants.py` | 构建词形 / 变形数据 |
| `tools/build_wordbook.py` | 生成单词书数据 |
| `tools/check_dict.py` | 检查词典格式、排序与索引 |
| `tools/dict_probe.py` | PC 端复刻设备查词算法 |
| `tools/gen_font.py` | 根据汉字表重新生成内置 14px 字体 |
| `tools/gen_read_fonts.py` | 生成 16/18/20/22px 2bpp 字库并打包 `build/font.bin` |
| `tools/gen_lunar_table.py` | 重新生成农历数据表（1900–2099） |
| `tools/png2lvgl.py` | PNG 图标转换为 LVGL C 数组 |
| `tools/motion3d/` | 电脑端姿态 3D 显示（UDP + PyQt5）与 STEP→OBJ 转换 |

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
- 横竖屏方向是**一个全局值**：除时钟、日历、游戏页（锁定横屏）外，所有页面共用，标题栏右端的小图标切换，方向持久化到 NVS 的 `rot`。旧配置里的 `rot_bookshelf` / `rot_words` / `rot_music` 会自动迁移到 `rot`。竖屏下页面按 240x320 重新排版（列表行、卡片、键盘、弹层都跟随当前逻辑分辨率）。
- 运行时 PNG / JPEG 解码未启用，照片和封面优先使用预转换的 RGB565 RAW / C 数组。
- 小智 AI 依赖网络和激活码，没有离线唤醒词。
- 温湿度目前只有 CSV 记录，没有历史图表页。
- 真机长跑回归仍需覆盖：内存水位、SD 热插拔、音乐 + 翻页、AI 与音频通道切换。
- 新增功能前需要关注 app 分区余量（factory 6 MB）。
- **改分区表后必须完整烧录一次**（CI 面板烧分区表 + `build/font.bin`；`build.ps1` 同样会带）；局域网 OTA 只更新 app，不会带新分区表和字库。
- 字库是 **2bpp**：8MB PSRAM 里代码 XIP 占了 4.4MB，只剩约 2.5MB；4bpp 的 20px 字库加载峰值 5MB 装不下。SD 卡 `/fonts` 优先；SD 上的字库太大/损坏时会自动改用 Flash 里的 2bpp；两者都没有时只有 14px。
- 词库只从 SD 卡 `/dict` 读取；没插 SD 时查词不可用。
- CI 构建的 artifact 不含字库镜像；用 `python tools/gen_read_fonts.py` 本地生成
  `build/font.bin`，CI 面板烧录时会自动带上。
- 姿态广播只驱动模型旋转；加速度积分位移漂移太大，已去掉。

---

## 版本与发布

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
