# Implement AI Functionality

> **Workspace:** d:\#MORUA\danpianji\ESP32-C5-LCD2.8-Touch-Reader

---

**User**

AI功能怎么做，详情见文档

**Assistant**

我先看一下需求文档里关于「AI 功能」的部分。

## 先对齐一件事：文档里其实没有 AI 的实现细节

我把项目文档翻了一遍，AI 只出现两处：

- [新需求.md](file:///d:/#MORUA/danpianji/ESP32-C5-LCD2.8-Touch-Reader/新需求.md#L2)：新增功能里有「小智AI」，没有展开
- [新需求.md](file:///d:/#MORUA/danpianji/ESP32-C5-LCD2.8-Touch-Reader/新需求.md#L16)：功能菜单卡片「书架、音乐、游戏、AI」

[项目.md](file:///d:/#MORUA/danpianji/ESP32-C5-LCD2.8-Touch-Reader/项目.md) 里完全没有 AI 章节，[技术.txt](file:///d:/#MORUA/danpianji/ESP32-C5-LCD2.8-Touch-Reader/技术.txt) 只有开发环境两行。代码侧入口已经在：[ui_func.c](file:///d:/#MORUA/danpianji/ESP32-C5-LCD2.8-Touch-Reader/main/ui/ui_func.c#L30-L34) 的 AI 卡片会跳 `PAGE_AI`，但 [app_navigator.c](file:///d:/#MORUA/danpianji/ESP32-C5-LCD2.8-Touch-Reader/main/app/app_navigator.c#L37-L64) 的页面表里没有 `PAGE_AI` 的 build 函数，点进去是「待实现」占位页。

如果你说的"文档"是另一份（比如飞书文档），把内容或链接给我，我按它来。

## 调研结论（决定方案走向的 4 个硬事实）

1. 「小智AI」= 开源的 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)。端侧只做**唤醒 + 音频编解码 + WebSocket**，ASR/LLM/TTS 全在服务端。
2. **ESP32-C5 能跑**：C5 从 v2.0 起被支持；`你好小智` 有 WakeNet9s 降配模型（专门为无 SIMD 的 C3/C5 出的），所以离线唤醒在 C5 上理论可行。
3. **但没有现成固件**：微雪的「小智 AI 适配产品列表」只有 C6 / P4 / S3 系列，没有 ESP32-C5-Touch-LCD-2.8。要跑小智就得自己写 board 配置（引脚、ES8311 mic 增益、显示）。
4. **两套固件不能共存**：微雪明确说刷小智后原程序不能用、不能一键切换。所以「保留伪装时钟 + 加 AI」只剩**集成进现有工程**这一条路。

## 建议的推进顺序

**第一步（推荐先做）：文字对话**——成本最低，和现有代码几乎零冲突
- 新服务 `srv_ai.c`：`esp_http_client` 打 DeepSeek/通义等 OpenAI 兼容接口（证书用 `crt_bundle`，时间靠现成的 `srv_ntp`），响应缓冲放 PSRAM。先非流式，稳了再上 SSE 增量显示。
- 新页面 `ui_ai.c`：气泡消息列表（可滚动）+ 输入框（直接复用 `ui_text_input_show()` 屏幕键盘）+ 发送按钮；上下文只留最近 N 轮在内存。
- 分区表不动（app 还空 59%）。

**第二步（可选）：语音朗读输出**——云端 TTS 返回 MP3，项目里已经有 `esp_audio_codec` / `esp_audio_simple_player` 依赖（`srv_player` 在用），解码 + ES8311 播放可以直接复用。

**第三步（要大动，想清楚再上）：语音输入**
- B1 接小智协议：抽它的 websocket 二进制协议 + Opus + 设备激活（6 位码绑 xiaozhi.me），能用官方服务端，不用自己写后端。代价：自己适配 C5 板级、加 esp-sr/opus 组件、app 分区要扩到 8MB（flash 32MB 够，但改 `partitions.csv` 后要整片烧录）。
- B2 按住说话：录音上传云端 ASR → 文本走第一步的链路。省掉唤醒词和 Opus，C5 单核压力小很多。

**风险**：C5 单核 240MHz、无 SIMD，唤醒 + Opus 编解码 + LVGL + 现有服务同时跑会比较吃紧，而且这条路上没有官方板级参考。

下面几件事定了我就能动手。

明白，那就移除 AI 入口。

AI 入口已移除。

**改动**
- [ui_func.c](file:///d:/#MORUA/danpianji/ESP32-C5-LCD2.8-Touch-Reader/main/ui/ui_func.c#L36-L55)：删掉 AI 卡片和 `on_ai` 回调；录音机从半卡变成整卡，布局现在是「书架 | 音乐」/「游戏 | 录音机」两行各两张等宽卡
- [app_navigator.h](file:///d:/#MORUA/danpianji/ESP32-C5-LCD2.8-Touch-Reader/main/app/app_navigator.h#L14)：删掉 `PAGE_AI` 枚举（它本来也没在页面表里注册，点了就是「待实现」占位页），顺手把 `PAGE_FUNC` 注释里的 AI 去掉了

全工程再搜 `PAGE_AI` / `on_ai` 已无引用（只改了 ui_func.c 和 app_navigator.h 两个文件，没碰导航器的表）。

**两件残留，你定**
1. `ic_ai` 图标还在生成产物里（[ui_icons.h](file:///d:/#MORUA/danpianji/ESP32-C5-LCD2.8-Touch-Reader/main/assets/images/ui_icons.h#L11)、[ui_icons.c](file:///d:/#MORUA/danpianji/ESP32-C5-LCD2.8-Touch-Reader/main/assets/images/ui_icons.c#L153)），现在没人引用，约占 1KB flash。它是 `tools/png2lvgl.py` 从 `icon/` 目录生成的，要彻底清掉得删源图再重新生成——需要我处理吗？
2. 需求文档里还写着 AI：[新需求.md:2](file:///d:/#MORUA/danpianji/ESP32-C5-LCD2.8-Touch-Reader/新需求.md#L2) 和 [:16](file:///d:/#MORUA/danpianji/ESP32-C5-LCD2.8-Touch-Reader/新需求.md#L16)，要不要一并同步？

编译还是你自己跑：

```powershell
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'; idf.py --no-ccache build; if ($LASTEXITCODE -eq 0) { python -m esptool --chip esp32c5 -p COM14 -b 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size 32MB --flash-freq 80m 0x20000 build\clock_reader.bin }
```

---

**User**

能否为项目添加PNG解析，性能怎么样

**Assistant**

## 结论：能，而且依赖其实**已经在项目里了**，只是没启用

我查了依赖树和构建产物：

- [dependencies.lock](file:///d:/#MORUA/danpianji/ESP32-C5-LCD2.8-Touch-Reader/dependencies.lock#L66-L99) 里 `espressif/esp_lv_decoder` v0.4.3 已经存在（`esp_lvgl_adapter` 的 public 依赖），它带 **libpng + zlib + esp_new_jpeg**，targets 明确包含 `esp32c5`
- 已经编译出来了：`build/esp-idf/espressif__esp_lv_decoder/liblvgl_docoder_lib.a`、`libespressif__libpng.a`、`libespressif__zlib.a`
- 但 [clock_reader.map](file:///d:/#MORUA/danpianji/ESP32-C5-LCD2.8-Touch-Reader/build/clock_reader.map) 里搜不到 `esp_lv_decoder_init`/`pngread`，`main/` 里也搜不到任何引用 → **被 `--gc-sections` 丢掉了，当前固件零成本**。LVGL 自带的 lodepng/sjpg 也是关的（`CONFIG_LV_USE_LODEPNG is not set`）

启用只要两步：`esp_lv_decoder_init(&h)`（LVGL 起来之后调一次）+ 提供图源（SD 读出到 PSRAM 直接把内存指针当 src 就行，[decoder_info 会检查 PNG magic](file:///d:/#MORUA/danpianji/ESP32-C5-LCD2.8-Touch-Reader/managed_components/espressif__esp_lv_decoder/src/lvgl9/esp_lv_decoder.c#L482-L483)，不一定要 lv_fs 路径）。

**Flash 成本**（`size -t` 实测归档总 text 段）：libpng 139KB + zlib 53KB + decoder 14KB ≈ **206KB 上限**；只链解码路径实际大概 **120~150KB**。app 还剩 59%（约 3.4MB），无压力。

## 性能：软解，量级是「几十到上百毫秒一张」

C5 是单核 240MHz RISC-V、**无 SIMD**，PSRAM 还是 **Quad 80MHz**（不是 Octal），所以两端都受限。PNG 解一张图的成本 = zlib inflate（占大头）+ 逐行反滤波 + 转 ARGB8888 + 写进 PSRAM。

按输出像素量估算（**分析估算，不是实测**）：

| 图片尺寸 | ARGB8888 缓冲 | 首解耗时量级 |
|---|---|---|
| 180×240（书架封面） | 169 KB | 50~80 ms |
| 240×320（满屏） | 300 KB | 100~150 ms |
| 800×600（大照片） | 1.8 MB | 0.6~1.2 s |

同尺寸 JPEG 大约是 PNG 的 1/2~1/3 时间（`esp_new_jpeg` 也已在依赖里，但 C5 没有硬件 JPEG，纯软解）。

**三个必须注意的坑：**

1. **图像缓存默认是 0** —— `CONFIG_LV_CACHE_DEF_SIZE=0`、`LV_IMAGE_HEADER_CACHE_DEF_CNT=0`。这意味着 `lv_image` **每次重绘都会重新解码**，PNG 直接卡死。必须开 cache 或自己把解码结果缓存起来。
2. **PNG 一律解成 ARGB8888**（[decoder.c:516](file:///d:/#MORUA/danpianji/ESP32-C5-LCD2.8-Touch-Reader/managed_components/espressif__esp_lv_decoder/src/lvgl9/esp_lv_decoder.c#L516)），而我们是 RGB565 屏 → 每帧还要软件 alpha 混合。不透明的图建议解一次后自己转成 RGB565 存着，或者干脆离线转（就像现在的 `cover.raw`）。
3. **单核**：解码 100ms 期间 UI 会明显掉帧，最好放独立任务里解 + 先显示 loading。

## 建议

- **静态 UI 资源（图标、固定封面）**：继续走现在的 `tools/png2lvgl.py`，编译期转 A8，运行时零解码，这条路最快，别改。
- **运行时才拿到的图（SD 卡里的 PNG、下载的封面、文件管理器预览）**：用 `esp_lv_decoder`，PGN 尺寸压到屏幕量级（≤480×640），解一次缓存住，不要在重绘路径里解。
- 如果只是要「固定素材 + 低内存」，`esp_mmap_assets` 的 **SPNG**（PNG 编译期切成 16px 高的切片，解码时逐片来）RAM 占用极低，但它需要把素材打进 flash 分区的流程。

要不要我接上？如果要，告诉我用途（书架封面 / 文件管理器看图 / 其它），我倾向的做法是：`app_init` 里 init 解码器 + 开 `LV_CACHE_DEF_SIZE`（给个 1~2MB，PSRAM 吃得住），然后写个 `srv_img` 帮你把「SD 路径 → 解好的 RGB565 缓冲」封装成一次调用。也可以先加一个只测解码耗时的临时函数，你编译刷进去看真实数字再定方案。

---

**User**

添加照片菜单，文件读取/photos
制作转换脚本将照片压缩并转换为RGB565的raw格式的文件，GUI python，支持批量

**Assistant**

要做两件事：把照片入口加进菜单 + 写一个批量转换的 GUI 脚本。先把现有的 raw 加载和卡片布局摸清楚，再定方案。

---

**User**

等一下，要支持，PNG、JP（E）G、WEBP
设置中可设置密码才能进入照片
打开照片（全屏）左右滑动可切换照片

**User**

RAW是后缀，不是格式，参考封面图

**Assistant**

依赖和参考实现都确认了：Pillow 12.0.0 带 WEBP 支持、tkinter 8.6 可用；`cover.raw` 的规格是 [epub2ebf.py 的 convert_cover()](file:///d:/#MORUA/danpianji/ESP32-C5-LCD2.8-Touch-Reader/tools/epub2ebf/epub2ebf.py#L797-L823) —— RGB565 **小端**（低字节在前）、等比缩小不放大、宽高另外记录。

还有 4 个分叉点定下来我就开工：

收到，方案定了：320×240 留黑边、只列文件名、数字密码存 config.dat、照片卡在录音机左边。开始动手。

现在开工。转换脚本我并行交给子代理，固件部分我自己写。

我先阅读项目里的参考实现，了解现有风格和打包逻辑。

现在阅读 GUI 参考文件和命令行部分的实现。

Pillow 12.0.0 和 numpy 2.4.2 都可用。参考风格已掌握，开始建工具。

核心逻辑完成，现在写 GUI。

两个文件已建好。现在写临时测试脚本来验证（放在系统临时目录，不污染项目）。

唯一失败项是我的测试断言写错了（200×400 竖图缩到 120×240，高度正好铺满 240，所以顶部不是黑边）。修正断言后重跑。

全部通过。再补一个针对性验证：完整转换链路在 numpy 被屏蔽时的输出与正常一致。

再对 GUI 做一次构建冒烟测试（不进入主循环，验证所有控件与回调能正常创建）。

全部验证通过。清理临时测试脚本与编译缓存。

---

