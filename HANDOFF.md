# 交接文档 —— ESP32-C5 伪装时钟阅读器

> 给接手的新智能体：先通读第 1~4 节（规则与现状），再看第 8 节的待办分析。
> **中文输入法的现场进展在最后一节「第 12 节」，接手前必读。**
> 文档日期 2026-10-31，基线提交 `88ed22f`。

---

## 1. 项目是什么

Waveshare **ESP32-C5-Touch-LCD-2.8** 上的一台「伪装时钟」掌上阅读器：

- 默认显示时钟（时间/日期/电量/温湿度），点左上角进入主菜单才露出真实功能
- 阅读（TXT/EBF）、书架、音乐、电台（中国之声 HLS）、录音、照片、单词、设置、游戏
- 屏幕横屏 **320×240**（面板物理是 240×320 竖的，靠 swap_xy + mirror 转过来）
- 硬件：ESP32-C5 单核 RISC-V 240MHz、8MB PSRAM、32MB Flash、SD 卡、ES8311 音频、SHTC3、QMI8658、PCF85063A、锂电池
- 框架：**ESP-IDF v6.1** + **LVGL 9.5.0**（已从 9.3.0 升级，见第 12 节）+ `espressif/esp_lvgl_adapter` 0.4.x
- 另有 PC 模拟器 `sim/`（MinGW + LVGL Windows GDI 后端），不依赖真机即可调 UI、跑电台/录音/校时

---

## 2. 当前 Git 状态

```
88ed22f  新增 EBF Studio 素材工作台，优化照片列表与时钟页刷新   ← 当前 HEAD (master)
97e2af8  初始化仓库：ESP32-C5 伪装时钟阅读器基线
```

- `88ed22f` 之后工作区**有大量未提交改动**（中文输入法接入进行中，见第 12 节）。
  这些改动**尚未提交**，也**不要自主提交**。
- 只有 `master` 分支，**没有远程**。
- `.gitignore` 排除了 `build/`、`sim/build/`、`managed_components/`、`words/ecdict.csv`、生成的 `dict.dat/idx`、`sim/sd` 大媒体、`*.bak`、`sim/sd/*.dat`、日志。
- 大数据文件（词库、媒体、照片）不入库，靠 `tools/` 脚本重建。

---

## 3. 硬性规则（违反会被用户直接打断）

1. **绝对不要自己执行固件编译/烧录**（`idf.py build`、`esptool`）。
   用户经常在他自己的终端跑「一键编译刷入」，你再起一次会撞同一个 `build/` 目录把编译搅坏。
   改完代码就停，只把命令给他。模拟器（`sim/`）编译是允许的。
2. **不要自主提交 Git、不要建分支**。只有用户明确说「提交」才提交。
3. 用户是中文交流、中文注释；深色主题，主题色 `#1585C0`；讨厌闪烁和无谓动画。
4. **模拟点击结果不对时，先当成「用户动了鼠标」**，不要反复调坐标改代码；测试前提醒用户别碰鼠标键盘；注入用绝对坐标。
5. 方案有分歧或信息不足时**先问**，别猜（用户明确要求）。

---

## 4. 快速上手

### 固件编译 / 刷入（给用户执行的命令，不要自己跑）

```powershell
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
idf.py --no-ccache build

# 只烧 app（日常迭代够用）
python -m esptool --chip esp32c5 -p COM14 -b 460800 --before default-reset --after hard-reset `
  write-flash --flash-mode dio --flash-size 32MB --flash-freq 80m 0x20000 build\clock_reader.bin
```

- 必须 `--no-ccache`（沙箱下 ccache 会卡死）。
- 编译日志末尾若因 `.git/index.lock` 被拦而退出码 1，**编译其实成功了**，看 `build/clock_reader.bin` 时间戳。
- 串口是 USB-Serial-JTAG（COM14），本板没有 CH343。

### 模拟器

```powershell
cd sim
& "C:\Espressif\tools\make-4.0.3\bin\make.exe" -S . -B build -G "MinGW Makefiles"
& "C:\Espressif\tools\make-4.0.3\bin\make.exe" --build build -j
.\build\clock_sim.exe
```

- 工程路径含 `#`，**必须先显式 configure** 再 build，否则 CMake 自动 reconfigure 会失败。
- 模拟器已实现真联网 HTTP、录音、NTP 存根，可以真跑中国之声与词典查询。
- 模拟器与真机共用 `main/services/` 和 `main/ui/` 的同一份代码，只有底层存根不同（`sim/stubs/`）。

### 常用验证脚本

```powershell
python tools/check_dict.py          # 词典格式校验
python tools/dict_probe.py          # 用 Python 复刻设备端查词算法，跑代表用例
python tools/gen_font.py ui_14      # 重烘中文字体（改了 font/hanzi.txt 之后）
```

---

## 5. 代码地图

```
main/
├── app/            app_main、app_navigator（页面栈与注册）、app_init
├── services/       纯逻辑，不碰 LVGL（除少数回调）
│   ├── srv_config   34 项配置 → /config.dat
│   ├── srv_fs/sd    文件系统与 SD 挂载
│   ├── srv_reader   阅读引擎：TXT 整本进 PSRAM，EBF 按章节读
│   ├── srv_library  书架扫描
│   ├── srv_dict     离线词典（索引 + 查找/前缀/建议/直译）
│   ├── srv_musiclib / srv_player  音乐库扫描 + MP3 软解 + I2S
│   ├── srv_radio    中国之声 HLS 直播（网络，不读 SD）
│   ├── srv_audio    ES8311 音箱/麦克风、提示音、PCM 写入口
│   ├── srv_wifi / ntp / alarm / timer / recorder / photo / progress / battery / sensors / rtc / input / motion / safe
├── ui/             每页一个 .c，页面通过 app_navigator 注册
├── assets/         fonts（font_ui_14、font_ipa_14）、images/ui_icons.c、gbk_table.c
└── sim/            模拟器（stubs 实现 IDF API 的 PC 版）
tools/              转换与生成脚本（含新建的 EBF Studio）
dict/               dict.dat + dict.idx（生成物，不入库）
font/               HarmonyOS Sans SC + hanzi.txt 字符集
```

---

## 6. 必知的技术约定与坑

### LVGL

- **screen → root 两层**：root 固定 320×240（**不要用 `LV_PCT`**）+ `bg_opa = COVER`；滑入动画动 root，不动 screen。
- `esp_lv_adapter_lock(0)` 是「非阻塞试一次」，正式取锁用 `UI_LOCK_TIMEOUT_MS = 1000`。
- 页面定时器句柄走事件 user_data + `LV_EVENT_DELETE` 删除，**不要用 static 存**。
- static 对象指针销毁时置空，并**比对父对象**（切页是 `lv_obj_delete_async`，手快 A→B→A 会清掉新指针）。
- 排版不要自己算字宽：**丢给隐藏 label 排版 + 量高度 + 二分**（见 `ui_reader.c` 的 `fit_chars`）。
- 测量 label 要放屏幕外，不能用 `HIDDEN`（隐藏对象不参与布局）。
- 弹层挂 `lv_screen_active()`，回调里**先删弹层再调业务回调**。
- 长按后松手 LVGL 还会补一个 `CLICKED`，要吞掉。
- 封面/竖图显示用 `lv_image_set_rotation`（`COVER_ROTATION 2700` 逆时针 90°）。

### IDF v6.1 编译（`-Werror`）

- `format-truncation`：拼配置键/路径改成常量字符串表或 `srv_fs_join()` 限长拼接。
- `stringop-truncation`：`strncpy` 改 `snprintf`。
- `esp_app_desc.h` 需要 `REQUIRES esp_app_format`。
- 链接报 `ParseException: Parsing sections info ...`：删掉坏的 `build/esp-idf/<组件>/lib*.a` 重编。

### BSP / 硬件

- `bsp_audio_codec_microphone_init()` **只有声明没有实现**，录音的 `mic_create()` 是照 speaker 的写法自己搭的。
- 触摸 `esp_lcd_touch` 是**先镜像后 swap**，`x_max/y_max` 填交换前的原生轴长。
- 电池：`Vbat = adc × 3.3 × 3 / 1023`，走放电曲线，非线性的。
- 中文文件名要 `CONFIG_FATFS_CODEPAGE_936` + `UTF-8` API 编码；串口输入中文路径不可靠。

### 关键的「总线事实」

**SD 卡和 LCD 共用同一个 SPI 主机 `SPI2_HOST`**：

- `components/esp32_c5_touch_lcd_2_8/include/bsp/display.h`：`BSP_LCD_SPI_NUM = SPI2_HOST`
- `esp32_c5_touch_lcd_2.8.c` 的 `bsp_sdcard_sdspi_mount()` 也用 `BSP_LCD_SPI_NUM`
- 触摸走 I2C，不冲突；WiFi 与 SPI 无关。

这意味着 **SD 大块读会阻塞 LCD flush，LCD 全屏刷新也会阻塞 SD 读**（SPI 总线互斥）。
第 8.5 节的音频卡顿分析以此为前提。

---

## 7. 已完成（不要重复实现）

- 时钟伪装层、主菜单、功能菜单、安全模式（持久化）、日历、电池页
- 阅读器（TXT/EBF、左右点击+滑动翻页、章节跳转、长按菜单、封面/简介）
- 书架（双列卡片、长按菜单、移出书架）
- 音乐（`srv_musiclib` + `srv_player`，后台播放控制条）
- 电台（中国之声 HLS，真拉流真解码）
- 录音机、照片（列表序号跳转、全屏左右滑、预读）、计时器/倒计时、闹钟
- 单词（查词/译双模式、前缀联想、拼写建议、详情徽章、离线词级直译）
- 设置八个二级页、网络页（扫描 + 添加网络 + 密码键盘）
- 文件管理器（`ui_files.c`）、多个小游戏
- 字体：`font_ui_14`（8242 汉字）+ `font_ipa_14`（音标，Arial 烘）
- 工具：`tools/studio`（PySide6 工作台，EPUB/图片转换 GUI）、`photo2raw --auto-rotate`

> `project_memory.md` 的「待做」有一处过时：文件管理器和音乐库早已实现，以代码为准。

---

## 8. 本轮待办（用户原话 + 分析）

用户这一轮列了 6 件事，其中 3 件是「要不要做」的决策项。**动手前先逐项确认优先级。**

### 8.1 提升 LVGL 版本 + 中文输入法（用户已决定接入）

**决定（2026-10-31）**：用户已确定接入 GitHub 的 `VanillaNahida/lvgl_pinyin_ime` 完整组件（键盘 + 候选栏），
不再考虑自研或只用引擎。该组件的硬性要求是 **LVGL 9.5**，所以 LVGL 必须升级。
中文词典需求用户选了 **C：中英双向互查**（见第 12.5 节）。

**先澄清一个事实**：LVGL 官方 `lv_keyboard` 只有 ASCII / 数字 / 特殊符号，**没有任何拼音或中文输入能力**。
所以「升级 LVGL → 就有中文输入法」不成立；升级与中文输入是两件独立的事，这次是接入组件才顺带升级。

现状：

- 版本锁在 `main/idf_component.yml`：已改为 `lvgl/lvgl: "9.5.0"`，`esp_lvgl_adapter: "^0.4.0"`
- 现有文本输入 `ui_text_input_show()`（`main/ui/ui_dialog.c`）是自绘英文键盘（`kb_map_lc` / `kb_map_uc` / `kb_map_sp`），
  只用于 WiFi 密码、书名搜索等 ASCII 场景。

候选路线：

| 方案 | 做法 | 成本 | 风险 |
|---|---|---|---|
| A. 自研拼音 IME | 屏幕键盘输拼音字母 + 拼音音节表 + 词库 → 候选栏 → 选中字写入 textarea | 中高 | 需要拼音数据与词库，内存占用要控制 |
| B. 接 `lvgl_pinyin_ime` 的引擎 | 只用其拼音引擎与数据，不用它的 UI | 中 | 仓库依赖的 LVGL 版本/API 需重新核实（**此前结论：UI 不要，引擎可单独用**） |
| C. 只做「手动输入 SSID」 | 不碰中文输入，新增一个 ASCII 输入框 | 低 | 中文 SSID 输不了 |

**建议**：先问清用户中文输入到底用在哪（隐藏 WiFi 的 SSID？文件名？查词？）。
如果只是连隐藏 WiFi，方案 C 足够；如果要做拼音输入，再评估 A/B。
**升级 LVGL 单独评估**：9.3 → 9.4/9.5 主要收益是 bug 修复与性能，需同步确认 `esp_lv_adapter` 兼容版本，
且所有页面回归一遍。不要为了输入法而升级。

### 8.2 单词查「相关词语、短语」

现状（`main/services/srv_dict.[ch]`）：

- `dict.dat` 5 字段：`word / phonetic / definition / flags / lemma`，15.5 万条，8.59MB
- 索引 `dict.idx` 28.5KB（每 64 行一个锚点 + 两字母桶），查词 3~8ms
- 接口：`lookup` / `lookup_lemma` / `prefix` / `suggest` / `translate`
- 词库**已包含多词短语**（`tools/build_dict.py` 的选词规则保留了短语），`prefix("app")` 现在就能列出 `apple pie` 这类条目——只是详情页没展示

可实现的方向：

1. **「相关短语」区块**（低成本）：详情页底部用 `srv_dict_prefix(当前词)` 再过滤掉自身，取前 5~10 条短语。
   纯索引扫描，无新数据、无新格式。
2. **词形变化**（低成本）：`lemma` 已有；构建脚本也处理了 exchange 变体。
   若要展示 `run/ran/running` 全表，需要在 `dict.dat` 增加字段或标签，属于格式变更。
3. **同义词/近义词**（高成本）：ECDICT 没有同义词关系。需要引入 WordNet（约 1.5MB 精简版）或自建常用同义词表，
   会增加固件/词库体积。**要先问用户是否接受**。

**建议**：先做 1，2 视需求；3 单独决策。

### 8.3 有中文输入就能连隐藏 WiFi

现状（已核实）：

- `main/ui/ui_set_net_add.c:111`：`if (ap->ssid[0] == '\0') return;` —— 扫描结果里**隐藏热点被直接丢弃**
- `main/services/srv_wifi.c:415`：扫描配置 `.show_hidden = false`
- `srv_wifi_connect(ssid, pass, save, manual)` 本身**支持任意 SSID**，包括隐藏网络（ESP-IDF 连接隐藏 AP 只要 SSID 非空即可，扫描才需要 show_hidden）
- 添加网络页目前只能「扫描列表 → 点一条 → 输密码」，没有手动输入 SSID 的入口

所以真正的缺口不是中文输入，而是**「手动输入 SSID」的界面**：

1. 在添加网络页加一行「手动添加」，弹 `ui_text_input_show()` 输 SSID，再输密码
2. 若 SSID 含中文，才需要中文输入法（否则 ASCII 就够）
3. 也可把 `show_hidden` 打开，让隐藏热点以「(隐藏网络)」占位出现在列表，点了再输 SSID
   —— 但 `srv_wifi_scan` 的回调里 `ssid` 为空，需要改 `srv_wifi_ap_t` 支持占位项

**建议**：先做「手动输入 SSID」，中文输入法按 8.1 单独决策。

### 8.4 章节跳转：章节多就卡很久，滑动也卡

现状（`main/ui/ui_reader.c`）：

- `goto_build_chapters()`（约 888 行）：
  1. **两趟遍历所有章节**调 `srv_reader_chapter_title()`；
  2. 把全部标题 `snprintf` 拼成一个 `\n` 分隔的大字符串（PSRAM）；
  3. `lv_roller_set_options()` 一次性塞给 roller。
- roller 内部要对**整个选项字符串**做文本布局/行高计算，几千行就是 O(n) 的文本处理；
  打开弹层那一下卡数百毫秒到数秒，滑动时也要在大文本里定位。

**先测量再改**：在 `goto_build_chapters` 的三段（遍历取标题 / 拼接 / `set_options`）各加一次
`esp_timer_get_time()` 日志，确认瓶颈在哪一段（多半是 `set_options`）。

候选方案：

| 方案 | 做法 | 效果 | 成本 |
|---|---|---|---|
| A. 虚拟列表（推荐） | 自建滚动容器，只建 7~8 行 label，滚动时按索引改文本（复用对象） | 打开 O(1)，滑动流畅 | 中 |
| B. 分块加载 | 先塞当前章前后 ~100 章，滚到边缘再追加 | 打开快，滚远要再加载 | 低 |
| C. 只显示章号 | 滚轮里只放「第 N 章」，确认后再查标题 | 打开快，但看不到章名 | 很低 |
| D. 缓存标题 + 截断 | 标题缓存进 PSRAM、每行截到 N 字符 | 仍有 O(n) roller 布局，治标 | 低 |

**建议**：A 或 B；先加测量日志拿到真实耗时，再决定。验收：2000 章的书，打开跳转弹层 <200ms，滑动不掉帧。

### 8.5 总线调度：放音乐会细微卡顿（用户怀疑 SD 读写冲突）

**事实**：怀疑是对的，但机制是 **SPI 总线互斥**，不是 SD 读写本身的冲突（见第 6 节）：

- SD 与 LCD 共用 `SPI2_HOST`
- 播放任务 `srv_player.c`：优先级 5、`IN_BUF_BYTES = 8192`、`PCM_BUF_BYTES = 8192`（最大 64KB）
  循环是 `fread 8KB → MP3 解码 → srv_audio_pcm_write（I2S DMA）`
- 每次 LVGL 刷新屏幕（单帧最多 320×240×2 = 150KB）都会长时间占住 SPI，
  播放任务的 `fread` 只能等；8KB 缓冲在 48kHz/立体声下只有约 20ms 音频，一被阻塞就断顿
- 另外 FATFS 有全局锁：书架扫描、进度保存、词典查词（`dict.dat` 在 SD 上）、照片扫描都会抢锁

**中国之声会不会卡**：电台走 WiFi + HLS，**不读 SD**，所以不会有 SPI 总线冲突；
但它仍然要解码 + 写 I2S + 与 LVGL 抢单核 CPU，网络抖动也会卡（已有起播缓冲 2 片、落后窗口重对齐）。

优化方向（按性价比排序）：

1. **先测量**：在 `player_task` 的 `fread` 与 `srv_audio_pcm_write` 前后各记一次耗时，串口统计最大值/分布；
   同时在 LVGL flush 前后记耗时，确认「谁阻塞谁」。
2. **把 SD 读与解码解耦**：单独「读卡任务」按 ring buffer 预读 2~4 秒音频，播放任务只消费；
   这样 SPI 偶发阻塞不会直接断音。
3. **加大缓冲**：`IN_BUF_BYTES` 8KB → 32~64KB（PSRAM），减少 `fread` 次数与锁竞争频率。
4. **整首预读进 PSRAM**（可选）：单曲 ≤4MB 时进 PSRAM，播放期间零 SD 访问；
   8MB PSRAM 要扣掉 LVGL 与照片/阅读占用，需要做上限与回退。
5. **优先级**：播放任务 5 可提到 6~7（注意别饿死 LVGL/IDLE；单核要留余量）。
6. **SD 访问串行化**：所有 SD 访问走同一个互斥锁，播放预读拿高优先级，避免 FATFS 全局锁抖动。

**验收**：真机播放 320kbps MP3 同时翻页/滑菜单，主观无断音；日志里单次 `fread` 耗时 P99 小于音频缓冲时长。

### 8.6 旋转屏幕（点击标题栏旋转，仅书架及子菜单、音乐及子菜单）

**历史结论（`project_memory.md` 记录）**：曾评估「横屏自适应（陀螺仪 + 标题栏旋转按钮）」并放弃：

1. 面板物理是 240×320，竖屏后逻辑分辨率变 240×320；
2. 现有页面全按 320×240 **绝对坐标写死**，等于 UI 重写一遍；
3. `esp_lv_adapter` **没有运行时切换 rotation 的 API**（0.4.x 只在注册显示时接受 `rotation` 参数；
   已核实 `include/` 里没有 set_rotation）。

**新需求范围**：只对「书架及子菜单」「音乐及子菜单」旋转，时钟页/阅读器等保持横屏。
（用户原话：「点击标题栏旋转，只有书架及子菜单、音乐及子菜单」）

技术路径：

| 路径 | 做法 | 可行性 |
|---|---|---|
| A. LVGL 软件旋转 | `lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90)`，LVGL 9 会自动交换逻辑宽高；flush 回调是否能跟着变需实测 `esp_lv_adapter` | 需实验 |
| B. 面板层旋转 | 给 BSP 增加 `bsp_display_get_panel()`，运行时 `esp_lcd_panel_swap_xy` + `esp_lcd_panel_mirror`，同步触摸 flags 与 LVGL 分辨率 | 改动可控，但 adapter 内部缓存的宽高/帧缓冲要一起处理 |
| C. 只改页面布局 | 面板不动，只在书架/音乐页「内部旋转排版」 | 不可行（字会躺倒） |

**必须先做可行性验证**（建议在 `sim/` 里先跑通 A 或 B 的机制，再上真机）：

- 切 rotation 时 `lv_display_set_resolution()` 与 flush 区域是否一致
- 触摸坐标是否自动跟随（`bsp_display_get_input_dev()`）
- 页面切换时是否有残影（LVGL 需要 `lv_obj_invalidate(lv_screen_active())`，必要时强制 full refresh）
- 标题栏旋转按钮放在 `ui_titlebar_create()` 里，但**只对指定页面显示**

**布局成本**：书架（双列卡片）与音乐页都要写一套竖屏布局，或改成 flex/百分比布局才能自适应。
竖屏可用高度仅 240px，书架一屏只能放 2 行卡片；音乐列表同理。需要改这些页的绝对坐标常量。

**建议**：先做 A/B 的最小验证（一个空白页切 rotation），确认机制可靠后再改书架/音乐页；
否则会陷入「页面改完发现底层不支持」的返工。

---

## 9. 之前计划过、一并需要问「要不要做」的候选

> 用户这轮说「之前我还有什么打算一并问了要不要做」。以下来自项目记忆 + 代码盘点，
> **逐项问用户要不要在本轮做**，不要擅自开始。

| 候选 | 说明 | 建议优先级 |
|---|---|---|
| 生词本 | 查词页收藏单词，需要存储格式与列表页 | 中 |
| 字母索引浏览 | 按 A~Z 浏览词库（`srv_dict` 已有两字母桶，改造量小） | 中 |
| 音标发音 | 需要 TTS 或预录音频，体积大；ESP32 端不现实 | 低（先问是否接受离线音库） |
| 自动锁屏 | 无操作 N 秒回时钟页（伪装场景很合理） | 高 |
| 低电量闪烁 | 电量低于阈值时电量图标闪烁提醒 | 中 |
| 照片缩略图网格 | `photo2raw` 额外生成 `.thumb`，列表改成网格选图；需重写照片列表 | 中 |
| 计算器 | 功能菜单里还没有 | 低 |
| 拼音输入法 | 见 8.1 | 待定 |
| 旋转屏幕 | 见 8.6 | 待定 |
| 隐藏 WiFi | 见 8.3 | 高（依赖 8.1 的结论） |
| 相关词语/短语 | 见 8.2 | 中 |

---

## 10. 交接时的第一步

按用户这轮的原话顺序，建议先问清这 6 个问题（用 AskUserQuestion，一次问完）：

1. **LVGL 升级**：目标版本？是否接受 `esp_lvgl_adapter` 兼容风险与全页面回归？
2. **中文输入法**：用在哪些场景（SSID / 文件名 / 查词 / 其它）？这决定做不做、做多大。
3. **隐藏 WiFi**：只做「手动输入 SSID」是否够？SSID 是否真的是中文？
4. **相关词语**：只做「相关短语」（低成本）还是也要「同义词」（需要新词库）？
5. **章节卡顿**：接受虚拟列表（彻底但中等改造）还是先分块加载（快但滚远要再加载）？
6. **旋转屏幕**：先做机制验证还是直接改书架/音乐布局？竖屏只放 2 行卡片能接受吗？

拿到答复后，再按优先级排期；每完成一项，都先在 `sim/` 验证，再让用户自己编译刷真机。

---

## 11. 一页速查：最容易踩的 10 个坑

1. 路径含 `#`：sim 必须先 configure 再 build
2. `idf.py` 必须 `--no-ccache`
3. 不要自己编译/烧录固件
4. 不要自主提交 Git
5. 排版用隐藏 label 量高度，别自己算字宽
6. 页面定时器句柄走 user_data，不要 static
7. static 指针销毁要比对父对象
8. 弹层挂 `lv_screen_active()`，先删弹层再回调
9. SD 与 LCD 共用 SPI2，播放卡顿先查总线竞争
10. `esp_lv_adapter` 没有运行时 rotation API，旋转先验证底层

---

## 12. 中文输入法接入进展（2026-10-31，进行中）

> 这一节是「现场笔记」：接手时先看这里，避免重复已经做过的事或踩同一个坑。

### 12.1 一句话状态

组件源码、LVGL 9.5、分区表、sdkconfig 都已落地；
**卡在生成工具的 Python 依赖（fonttools）**；词库与字体尚未生成，UI 尚未接入。

### 12.2 网络前提（很重要）

- Cloudflare WARP 服务在运行，模式是 `WarpWithDnsOverHttps`（流量 + DNS over HTTPS），符合用户要求。
- **git 全局代理 `http://127.0.0.1:26561` 端口没开**，直接用 `git` 会报
  `Failed to connect to github.com port 443 via 127.0.0.1`。
- 所有 GitHub 命令必须显式绕过代理：

```powershell
git -c http.proxy= -c https.proxy= clone ...
git -c http.proxy= -c https.proxy= ls-remote ...
```

- 已验证可用：`git ls-remote`、`git clone`（lvgl 与 lvgl_pinyin_ime 都成功）。
- `WebFetch` 工具也能直接读 GitHub 页面（读 README / docs 不用 clone）。

### 12.3 已经完成的改动（全部未提交）

| 文件/目录 | 改动 |
|---|---|
| `components/lvgl_pinyin_ime/` | 新增：上游 `VanillaNahida/lvgl_pinyin_ime` 完整源码（commit `1e2aa50840c838ebfb51e6e4756153f0084d52c3`），含 `src/third_party/libgooglepinyin`、`data/`（3.5MB 源词表）、`tools/`；没有 `.git` |
| `managed_components/lvgl__lvgl/` | 整目录替换为 **LVGL v9.5.0**（原 9.3.0）。该目录被 `.gitignore` 忽略，不入库 |
| `CMakeLists.txt` | 加 `EXTRA_COMPONENT_DIRS` 指向 `components/lvgl_pinyin_ime/src/third_party/libgooglepinyin`；设置 `LV_PINYIN_IME_EMBED_DICT=OFF`（不把 1MB 词库编进 app） |
| `main/idf_component.yml` | `lvgl/lvgl` 9.3.0 → **9.5.0** |
| `partitions.csv` | 新增 `dict`（data/0x40，`0x180000` = 1.5MB）与 `font`（data/0x41，`0x300000` = 3MB） |
| `sdkconfig.defaults` | `CONFIG_LV_FONT_FMT_TXT_LARGE=y`、`CONFIG_LV_USE_FS_MEMFS=y`、`CONFIG_LV_FS_MEMFS_LETTER=65`、`CONFIG_LV_PINYIN_IME_FONT_SRC_PARTITION=y`、`CONFIG_LV_PINYIN_IME_DICT_SRC_PARTITION=y`、`CONFIG_LV_PINYIN_IME_VOICE_ENABLE=n` |

**还没做但提交前必须做**：`.gitignore` 补 `components/lvgl_pinyin_ime/generated/`、
`components/lvgl_pinyin_ime/tools/venv/`、`components/lvgl_pinyin_ime/data/dict/dict_pinyin.dat`。

### 12.4 当前阻塞：生成工具装不上依赖

`components\lvgl_pinyin_ime\tools\setup_env.ps1` 报：

```
ERROR: No matching distribution found for fonttools>=4.50
pip install failed
```

原因：脚本的预设 IDF Python 路径都不存在，回退到系统 **Python 3.14.2**；
清华镜像（`pip config` 已指向它）没有满足该版本约束的 fonttools（pypinyin 同理）。
`tools/venv` 已创建，但里面没有依赖。

解决办法（任选其一）：

1. 指定 ESP-IDF 自带的 Python 3.11/3.12（推荐）：

```powershell
powershell -ExecutionPolicy Bypass -File components\lvgl_pinyin_ime\tools\setup_env.ps1 -Python <idf-python.exe 绝对路径>
```

   `setup_env.ps1` 里预设的 `D:\Espressif\EIM\tools\idf-python\3.11.2\python.exe` 不存在，
   需要先找实际路径（可能在 `D:\Espressif\python_env\...` 或 `C:\Espressif\...`）。

2. 或手动在现有 venv 里装：

```powershell
components\lvgl_pinyin_ime\tools\venv\Scripts\python.exe -m pip install fonttools pypinyin
```

   若 3.14 仍然装不上，只能换 Python。

### 12.5 生成词库与字体（还没跑）

在组件目录里执行：

```powershell
cd components\lvgl_pinyin_ime
.\tools\venv\Scripts\python.exe tools\gen_charset.py
.\tools\venv\Scripts\python.exe tools\gen_font.py --format bin
python tools\gen_dict.py
```

- `gen_dict.py` 会用 g++ 现场编译 `generated/dictbuilder`；本机 **g++ 16.1.0** 可用。
- Node.js **v24.18.0** 已安装（`gen_font.py` 的 lv_font_conv 依赖）。
- 产物：
  - `generated/lv_font_ime_20.bin`、`lv_font_ime_16.bin`，合并为 `generated/font_partition.bin`
  - `data/dict/dict_pinyin.dat`（上游实测 1,068,442 B）
- 当前是 `FONT_SRC_PARTITION`，构建**不会**检查 `generated/*.c`，
  但运行时必须烧 `font` 分区，否则候选栏全是方框（静默失败）。

烧录（**用户自己执行，不要代跑**）：

- `dict` 分区：`esptool write-flash <dict 偏移> data/dict/dict_pinyin.dat`
- `font` 分区：`esptool write-flash <font 偏移> generated/font_partition.bin`
- 偏移要先编译一次，再用 `idf.py partition-table` 查询（分区表是自动排布）。
- 也可用组件自带 `tools/flash_dict.ps1`（需要 IDF 环境）。

### 12.6 已知风险（编译前必须验证）

1. **ESP32-C5 不在组件官方支持列表**（官方是 S3 / P4，S31 preview）。
   组件是纯 C/C++，理论可编，但第一次构建要重点看 libgooglepinyin 在 RISC-V 工具链上的表现。
2. **`esp_lvgl_adapter` 0.4.3 与 LVGL 9.5**：adapter 依赖 `lvgl >=8,<10`，理论兼容，
   但 0.4.3 没针对 9.5 测过。若报 API 变更，需要小改 adapter（它位于 `managed_components`，
   重新拉依赖会被覆盖，补丁要记录在案）。
3. **app 分区 6MB**：当前固件 3.26MB。词库/字体已放分区，组件代码预计只增加 200~400KB，够用。
   **千万不要临时改回字体内嵌**（+3.2MB 直接爆）。
4. **`usr` 分区缺失**：Kconfig 有 `LV_PINYIN_IME_USER_PARTITION="usr"`，
   `src/data/ime_dict.c` 会返回 `/usr/ime_user.dat`，但分区表没有 `usr`。
   上游 README 注明用户词库持久化「尚未落地」，启动时留意日志即可。
5. **sim 也指向 9.5**：`sim/CMakeLists.txt` 直接用 `managed_components/lvgl__lvgl`，
   已跟随 9.5，但**还没编译验证**（这是下一步最省力的验证手段）。

### 12.7 下一步顺序（建议）

1. 解决 fonttools（12.4）
2. 生成 charset / font / dict（12.5）
3. **编译 sim** 验证 LVGL 9.5 与项目代码兼容（允许自己跑）
4. 让用户编译固件验证 C5 + adapter（不要自己跑），烧 `dict`/`font` 分区
5. 写 UI 适配层 `main/ui/ui_ime.[ch]`：封装 `lv_pinyin_ime`，
   先接 WiFi「手动输入 SSID/密码」（同时解决隐藏 WiFi），再接书名搜索框
6. 中英双向词典（12.8）

### 12.8 中文词典 C：中英双向互查（用户已选）

目标：汉字/拼音 → 英文；英文 → 中文（后者现有 ECDICT 已覆盖）。

建议数据源：**CC-CEDICT**（开源汉英词典，约 12 万条，含汉字词、拼音、英文释义，文本约 4MB）。
离线转换方案：

- 新增 `tools/build_zh_dict.py`：筛选常用词，生成 `zhdict.dat`（词条 + 拼音 + 英文释义）+ `zhdict.idx`
  （复用 `srv_dict` 现有的稀疏索引格式，每 64 行锚点 + 两字母桶）
- 新增 `srv_dict_zh.[ch]`（或扩展 `srv_dict`）：`lookup_zh()` / `prefix_zh()` / `pinyin_lookup()`
- 存储位置：优先新增 data 分区（32MB Flash 有余量）；放 SD 会与 LCD 抢 SPI2，查词更卡
- UI：查词页现有「查词/译」双模式，可扩展为自动识别输入语言（中文 → 中英互查；英文 → 英汉）
- 体积控制：只保留常用 3~5 万条，控制在 2~3MB

**动手前先问用户**：可接受体积（2MB / 3MB / 全量）；是否要拼音查词（CC-CEDICT 自带拼音字段，能做）。

### 12.9 提交前清单

- 补 `.gitignore`（见 12.3）
- 决定 `components/lvgl_pinyin_ime` 是否入库（含 3.9MB 源词表）；
  若不入库，必须在文档里写清获取方式与固定 commit
- 确认 `managed_components` 仍被忽略（LVGL 9.5 不随仓库走）
- 向用户确认后再提交（本仓库规则：不自主提交）
