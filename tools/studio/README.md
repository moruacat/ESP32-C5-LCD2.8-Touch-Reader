# EBF Studio

ESP32-C5 触摸阅读器的素材工作台。把 `tools/epub2ebf` 和 `tools/photo2raw`
两个转换器整合进同一个现代深色桌面界面，转换逻辑仍然复用原来的 Python 脚本。

## 功能

**EPUB → EBF**
- 单本 / 批量转换，文件夹递归扫描
- 跳过前 N 章（去掉简介、前言、目录页）
- 覆盖已有输出目录
- 段间空行、去掉正文开头重复的章标题
- 实时章节进度与日志

**图片 → RGB565 RAW**
- 多选图片或整个文件夹
- 按 320×240 等比缩放、黑底居中，输出 153600 字节小端裸数据
- 自动旋转（默认开启）：竖图逆时针转 90° 后能显示更多像素才旋转，横图 / 方图保持原样
- 同名文件自动加序号，或手动开启覆盖
- 选中图片后按真实 4:3 比例预览

**通用**
- 文件和文件夹都可以直接拖进虚线框
- 转换在 QThread 后台运行，界面不卡顿；停止会在当前文件处理完后生效
- 成功后可直接打开输出目录
- 选项和窗口位置自动记忆（QSettings，写入 `HKCU\Software\MORUA\EBFStudio`）
- Windows 深色标题栏、任务栏独立图标

## 运行

```powershell
python -m pip install -r requirements.txt
python main.py
```

也可以双击 `run.bat`（无控制台窗口）。需要排错时用 `python main.py` 启动，
错误会打印并弹窗。

要求 Python 3.10+（本机实测 3.14.2）、PySide6 6.12+、Pillow。
Pillow 只在转换封面和图片时用到；缺失时 EPUB 正文仍可转换，但没有封面。

## 目录结构

```
tools/studio/
├── main.py                 入口
├── run.bat                 双击启动
├── requirements.txt
└── app/
    ├── __init__.py         版本号
    ├── theme.py            配色 + 全局 QSS
    ├── icons.py            内联 SVG 图标与窗口图标
    ├── widgets.py          卡片、拖拽区、开关、步进器、日志、预览
    ├── workers.py          两个转换器的 QThread 包装
    ├── window.py           主窗口与侧边栏
    └── pages/
        ├── base.py         滚动容器、文件对话框、设置
        ├── epub.py         EPUB → EBF 页面
        ├── photo.py        图片 → RAW 页面
        └── about.py        关于 / 环境信息
```

## 和原来的界面是什么关系

- `tools/epub2ebf/epub2ebf_gui.py`、`tools/photo2raw/photo2raw_gui.py` 原样保留，
  仍然可以单独运行。
- 新界面不复制转换代码，直接调用：
  - `epub2ebf.collect_inputs()` / `epub2ebf.convert_one()`
  - `photo2raw.collect_inputs()` / `photo2raw.convert_one()`
- 修改转换规则（比如封面尺寸、章节切分）只需要改原来的脚本，新界面自动生效。
- `photo2raw` 新增了 `--auto-rotate` 命令行开关（默认关闭以保持旧行为）；
  桌面应用里的“自动旋转”默认开启。

## 输出到设备

- 书籍：`<书名>.ebf/` 拷进 SD 卡 `/books/`
- 图片：`.raw` 拷进 SD 卡 `/photos/`

## 打包成 exe（可选）

```powershell
python -m pip install pyinstaller
pyinstaller --noconsole --name EBFStudio ^
    --paths ..\epub2ebf --paths ..\photo2raw main.py
```

`--paths` 让 PyInstaller 能找到同级的两个转换器模块。
