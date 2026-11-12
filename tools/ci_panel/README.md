# CI 固件助手

Windows + PySide6 的 GitHub Actions 小工具，用于：

- 查看指定仓库/分支最近的 Actions 构建
- 查看失败或成功运行的完整日志
- 下载最新成功构建的 `firmware` artifact
- 选择串口并烧录刚下载的固件
- 一键把当前本地分支推送到 `origin`（只推送已提交内容，不会自动提交；
  没有新 commit 时会明确提示，不会把未提交改动算进去）

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

烧录前会再次确认，烧录过程中请勿断开设备。
