# -*- coding: utf-8 -*-
"""主窗口：侧边栏 + 页面容器 + 窗口级设置。"""

from __future__ import annotations

import ctypes
import sys

from PySide6.QtCore import QSettings
from PySide6.QtGui import QGuiApplication
from PySide6.QtWidgets import (
    QFrame, QHBoxLayout, QLabel, QMainWindow, QMessageBox, QStackedWidget,
    QVBoxLayout, QWidget,
)

from . import __version__, icons, theme
from .pages import AboutPage, EpubPage, PhotoPage
from .widgets import Divider, NavButton


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.settings = QSettings("MORUA", "EBFStudio")
        self._dark_title_done = False

        self.setWindowTitle("EBF Studio — ESP32 素材工作台")
        self.setWindowIcon(icons.app_icon(128))
        self.setMinimumSize(980, 660)

        root = QWidget()
        root.setObjectName("root")
        self.setCentralWidget(root)

        row = QHBoxLayout(root)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(0)

        self.pages = [EpubPage(), PhotoPage(), AboutPage()]

        row.addWidget(self._build_sidebar())

        content = QWidget()
        content.setObjectName("content")
        content_box = QVBoxLayout(content)
        content_box.setContentsMargins(0, 0, 0, 0)
        content_box.setSpacing(0)

        self.stack = QStackedWidget()
        for page in self.pages:
            self.stack.addWidget(page)
        content_box.addWidget(self.stack)
        row.addWidget(content, 1)

        self.nav_buttons[0].setChecked(True)
        self.stack.setCurrentIndex(0)

        self._restore_geometry()

    # ---------------- 侧边栏 ----------------

    def _build_sidebar(self) -> QFrame:
        sidebar = QFrame()
        sidebar.setObjectName("sidebar")
        sidebar.setFixedWidth(236)

        box = QVBoxLayout(sidebar)
        box.setContentsMargins(16, 22, 16, 16)
        box.setSpacing(6)

        brand_row = QHBoxLayout()
        brand_row.setSpacing(10)
        brand_row.setContentsMargins(4, 0, 0, 0)

        logo = QLabel()
        logo.setPixmap(icons.app_icon(80).pixmap(38, 38))
        logo.setFixedSize(38, 38)
        brand_row.addWidget(logo)

        brand_text = QVBoxLayout()
        brand_text.setSpacing(1)
        name = QLabel("EBF Studio")
        name.setObjectName("brand")
        sub = QLabel("ESP32 素材工作台")
        sub.setObjectName("brandSub")
        brand_text.addWidget(name)
        brand_text.addWidget(sub)
        brand_row.addLayout(brand_text, 1)
        box.addLayout(brand_row)

        box.addSpacing(22)
        box.addWidget(self._section("工具"))

        self.nav_buttons: list[NavButton] = []
        self._add_nav(box, "book", "EPUB → EBF", 0)
        self._add_nav(box, "image", "图片 → RAW", 1)

        box.addSpacing(12)
        box.addWidget(self._section("其他"))
        self._add_nav(box, "info", "关于", 2)

        box.addStretch(1)
        box.addWidget(Divider())

        status_row = QHBoxLayout()
        status_row.setContentsMargins(6, 8, 6, 2)
        status_row.setSpacing(8)
        dot = QLabel()
        dot.setFixedSize(8, 8)
        dot.setStyleSheet(
            f"background: {theme.OK}; border-radius: 4px;")
        status_row.addWidget(dot)
        status = QLabel("转换引擎就绪")
        status.setStyleSheet(f"color: {theme.FAINT}; font-size: 11px;")
        status_row.addWidget(status, 1)
        box.addLayout(status_row)

        version = QLabel(f"v{__version__}")
        version.setStyleSheet(f"color: #4C5567; font-size: 10px; padding-left: 22px;")
        box.addWidget(version)

        return sidebar

    def _section(self, text: str) -> QLabel:
        item = QLabel(text)
        item.setObjectName("sectionLabel")
        return item

    def _add_nav(self, box: QVBoxLayout, icon_name: str, text: str, index: int) -> None:
        button = NavButton(icon_name, text)
        button.clicked.connect(
            lambda _checked=False, i=index: self._switch(i))
        self.nav_buttons.append(button)
        box.addWidget(button)

    def _switch(self, index: int) -> None:
        self.stack.setCurrentIndex(index)
        self.nav_buttons[index].setChecked(True)

    # ---------------- 窗口 ----------------

    def _restore_geometry(self) -> None:
        geometry = self.settings.value("window/geometry")
        if geometry is not None and self.restoreGeometry(geometry):
            return

        screen = QGuiApplication.primaryScreen()
        if screen is None:
            self.resize(1180, 800)
            return

        area = screen.availableGeometry()
        width = min(1180, int(area.width() * 0.92))
        height = min(800, int(area.height() * 0.92))
        self.resize(width, height)
        self.move(area.x() + (area.width() - width) // 2,
                  area.y() + (area.height() - height) // 2)

    def showEvent(self, event) -> None:
        super().showEvent(event)
        if not self._dark_title_done:
            self._dark_title_done = True
            self._apply_dark_titlebar()

    def _apply_dark_titlebar(self) -> None:
        """Windows 10/11：让系统标题栏跟着深色主题走。"""
        if sys.platform != "win32":
            return
        try:
            hwnd = int(self.winId())
            dwm = ctypes.windll.dwmapi

            def set_attr(attr: int, value: int) -> None:
                data = ctypes.c_int(value)
                dwm.DwmSetWindowAttribute(
                    hwnd, attr, ctypes.byref(data), ctypes.sizeof(data))

            set_attr(20, 1)          # DWMWA_USE_IMMERSIVE_DARK_MODE
            set_attr(34, 0x00241A0E)  # 边框：接近 #0E1A24
            set_attr(35, 0x00140E0B)  # 标题栏背景：#0B0E14
            set_attr(36, 0x00F5EDE9)  # 标题文字：#E9EDF5
        except Exception:
            pass

    def closeEvent(self, event) -> None:
        busy = [page for page in self.pages if page.is_busy()]
        if busy:
            box = QMessageBox(self)
            box.setWindowTitle("正在转换")
            box.setText("还有任务在运行")
            box.setInformativeText("请先点击页面里的「停止」，等当前文件处理完再退出。")
            box.setIcon(QMessageBox.Information)
            box.setStandardButtons(QMessageBox.Ok)
            box.button(QMessageBox.Ok).setText("知道了")
            box.exec()
            event.ignore()
            return

        self.settings.setValue("window/geometry", self.saveGeometry())
        event.accept()
