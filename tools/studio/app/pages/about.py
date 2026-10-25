# -*- coding: utf-8 -*-
"""关于 / 环境信息页。"""

from __future__ import annotations

import platform
from pathlib import Path

from PySide6.QtCore import Qt, QUrl
from PySide6.QtGui import QDesktopServices
from PySide6.QtWidgets import QGridLayout, QHBoxLayout, QPushButton, QVBoxLayout, QWidget

from .. import __version__, icons, theme
from ..widgets import Card, Divider, PageHeader, label
from .base import BasePage

TOOLS_DIR = Path(__file__).resolve().parents[3]


class AboutPage(BasePage):
    def __init__(self, parent=None):
        super().__init__(parent)
        self._build()

    def _build(self) -> None:
        header = PageHeader(
            "关于",
            "EBF Studio 是 ESP32-C5 触摸阅读器的素材准备工具，界面与转换逻辑分离，"
            "原来的命令行脚本仍然可以独立使用",
        )
        self.column.addWidget(header)

        brand = Card()
        row = QHBoxLayout()
        row.setSpacing(16)

        logo = label("")
        logo.setPixmap(icons.app_icon(96).pixmap(52, 52))
        row.addWidget(logo)

        texts = QHBoxLayout()
        texts.setSpacing(10)
        left = QWidget()
        box = QVBoxLayout(left)
        box.setContentsMargins(0, 4, 0, 4)
        box.setSpacing(3)
        name = label("EBF Studio")
        name.setStyleSheet(f"color: {theme.TEXT}; font-size: 17px; font-weight: 700;")
        box.addWidget(name)
        box.addWidget(label(f"版本 {__version__} · 深色界面 · PySide6", "caption"))
        box.addStretch(1)
        texts.addWidget(left, 1)

        self.open_tools = QPushButton("打开 tools 目录")
        self.open_tools.setProperty("variant", "ghost")
        self.open_tools.setIcon(icons.icon("folder", theme.MUTED, 15))
        self.open_tools.setCursor(Qt.PointingHandCursor)
        self.open_tools.clicked.connect(
            lambda: QDesktopServices.openUrl(QUrl.fromLocalFile(str(TOOLS_DIR))))
        texts.addWidget(self.open_tools, 0, Qt.AlignVCenter)

        row.addLayout(texts, 1)
        brand.add_layout(row)
        self.column.addWidget(brand)

        # 运行环境
        env = Card("运行环境")
        grid = QGridLayout()
        grid.setHorizontalSpacing(24)
        grid.setVerticalSpacing(9)
        grid.setContentsMargins(0, 2, 0, 2)

        rows = [
            ("Python", platform.python_version()),
            ("PySide6", self._pyside_version()),
            ("Pillow", self._pillow_version()),
            ("转换器", "tools/epub2ebf · tools/photo2raw"),
            ("目标设备", "ESP32-C5 · 240 × 320 · RGB565"),
        ]
        for i, (key, value) in enumerate(rows):
            key_label = label(key, "caption")
            key_label.setFixedWidth(78)
            grid.addWidget(key_label, i, 0, Qt.AlignTop)
            value_label = label(value)
            value_label.setStyleSheet(f"color: {theme.TEXT}; font-size: 12px;")
            value_label.setWordWrap(True)
            grid.addWidget(value_label, i, 1)
        grid.setColumnStretch(1, 1)
        env.add_layout(grid)
        self.column.addWidget(env)

        # 输出规格
        spec = Card("输出规格")
        spec.add(label("EPUB → EBF", "cardTitle"))
        spec.add(label(
            "每本书生成一个 <书名>.ebf 文件夹：chapter.N.ebfl（正文）、"
            "chapters.ebfl（章名索引）、data.ebfl（XML 元数据）、cover.raw（240×320 RGB565 封面）。",
            "caption", wrap=True))
        spec.add(Divider())
        spec.add(label("图片 → RAW", "cardTitle"))
        spec.add(label(
            "等比缩放到 320×240 以内，居中贴到黑底画布，输出 153600 字节的 RGB565 小端裸数据，"
            "无文件头，直接放进 SD 卡 /photos。开启自动旋转时，竖图会逆时针转 90° 以占满屏幕。",
            "caption", wrap=True))
        spec.add(Divider())
        spec.add(label("存放位置", "cardTitle"))
        spec.add(label(
            "书籍：SD 卡 /books/　·　图片：SD 卡 /photos/　·　"
            "转换只读取源文件，不会修改或删除原始 EPUB / 图片。",
            "caption", wrap=True))
        self.column.addWidget(spec)

        # 使用提示
        tips = Card("小提示")
        for text in (
            "虚线框支持直接拖入文件或文件夹，批量转换会递归扫描子目录。",
            "转换在后台线程运行，界面不会卡住；停止会在当前文件处理完后生效。",
            "日志区域可以选中复制；设置会自动记住，下次打开沿用。",
            "原来的 tkinter 界面（epub2ebf_gui.py / photo2raw_gui.py）保持可用。",
        ):
            item = label("•  " + text, "muted", wrap=True)
            item.setStyleSheet(f"color: {theme.MUTED}; font-size: 12px;")
            tips.add(item)
        self.column.addWidget(tips)

        self.column.addStretch(1)

    @staticmethod
    def _pyside_version() -> str:
        try:
            from PySide6 import __version__ as version
            return version
        except Exception:
            return "未知"

    @staticmethod
    def _pillow_version() -> str:
        try:
            import PIL
            return PIL.__version__
        except Exception:
            return "未安装（封面/图片将无法转换）"
