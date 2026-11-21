# CI 固件助手

Windows + PySide6 的 GitHub Actions 小工具，用于：

- 查看指定仓库/分支最近的 Actions 构建
- 查看失败或成功运行的完整日志
- 下载最新成功构建的 `firmware` artifact
- 选择串口并烧录刚下载的固件
- 一键把当前本地分支推送到 `origin`（只推送已提交内容，不会自动提交；
  没有新 commit 时会明确提示，不会把未提交改动算进去）
- **估算完成时间**：取最近 10 次已完成运行的平均耗时，给进行中的构建算出
  「预计 HH:MM 完成（剩 N 分）」，显示在详情区和列表悬浮提示里。
  还在排队（`run_started_at` 为空）的运行不估算，避免误报
- **构建完成提醒**：自动刷新时发现某次运行从"进行中"变成"已完成"，会写日志、
  高亮状态栏、响一声提示音，并通过系统托盘弹气泡通知（托盘不可用时自动退化）。
- **局域网 OTA 推送**：用 mDNS 扫描 `_clockreader-ota._tcp` 找设备，把已下载的
  固件 POST 到设备；设备校验通过后自动重启并切到另一个 app 分区（factory ↔ ota_0）。
  接口、分区与回滚设计见 [`docs/ota_eval.md`](../../docs/ota_eval.md)。
  设备发现依赖 `zeroconf`，未安装时只影响这一项功能。

工具不会自动提交/推送代码，也不会自动烧录；所有推送、下载、烧录都由界面按钮触发。

推送依赖本机已安装 Git，并已配置好 `origin` 和 Git 凭据。

## 运行

```powershell
python -m pip install -r tools/ci_panel/requirements.txt
python tools/ci_panel/main.py
```

也可以双击 `tools/ci_panel/run.bat`。

## GitHub Token

- 下载 Actions artifact 必须填写 Token（GitHub 的 artifact 下载接口要求认证）。
- 细粒度 Token 勾选 **Actions: Read**；经典 Token 公开仓库可用 `public_repo`。
- 勾选「记住 Token」后，使用 Windows DPAPI 按当前 Windows 用户加密后保存在本机，
  不写入仓库。换用户或换机器后需要重新填写。
- 不勾选时只在当前进程内使用，也可以用环境变量 `GITHUB_TOKEN` 提供。

## 下载位置

固件会解压到：

```text
build/ci/run-<run_id>/
```

其中包含 `clock_reader.bin`、bootloader、分区表和 OTA 数据。`build/` 已在 `.gitignore` 中。

## 阅读字库（font 分区）

CI 构建没有 `flash_assets/`，所以 artifact 里通常没有 `font.bin`。字库在本地一键生成：

```powershell
python tools/gen_read_fonts.py
```

默认会生成：

- `font/font_ui_{16,18,20,22}.bin`：四档 2bpp 字库（共约 4.9MB）
- `flash_assets/fonts/`：打包输入
- `build/font.bin`：6MB SPIFFS 镜像（font 分区内容）

烧录时面板会优先用 artifact 里的 `font.bin`，没有就自动找项目根目录的
`build/font.bin`。**只有勾选「同时烧录阅读字库」才会写 font 分区**，默认不勾选，
这样日常刷 app 不会多传 6MB。确认框会显示烧录范围与字库状态。

另外新增「完整烧录」勾选项：

- 勾选（默认）：bootloader + 分区表 + OTA 数据 + app，首次刷或换分区表时用；
- 不勾选：只写 `0x20000` 的 app，日常改代码迭代最快。

两个勾选状态会记住，下次打开按上次的选择来。

> 分区表在 1.5.4 有变化（新增 font 分区）。第一次刷完新分区表后，若没烧
> `font.bin`，font 分区是空的，属于正常现象；再勾选字库重新烧一次即可。
> 词库只放 SD 卡，不参与烧录。

## 烧录

GUI 会自动查找以下位置的 `esptool.exe`：

```text
C:\Espressif\tools\python\*\venv\Scripts\esptool.exe
%USERPROFILE%\.espressif\python_env\*\Scripts\esptool.exe
```

也可以把 esptool 放进 `PATH`，或者在一个已安装 esptool 的 Python 环境里运行本工具。
找不到 esptool 时，构建查看和固件下载仍然可用。

烧录目标固定为 ESP32-C5、32MB Flash，烧录地址：

| 文件 | 地址 |
|---|---|
| bootloader.bin | 0x2000 |
| partition-table.bin | 0x8000 |
| ota_data_initial.bin | 0xf000 |
| clock_reader.bin | 0x20000 |
| font.bin（可选，本地生成） | 0x1220000 |

烧录前会再次确认，烧录过程中请勿断开设备。
