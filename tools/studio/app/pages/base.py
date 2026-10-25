# -*- coding: utf-8 -*-
"""页面基类：统一的滚动容器、文件对话框和设置存储。"""

from __future__ import annotations

from PySide6.QtCore import QSettings, Qt
from PySide6.QtWidgets import (
    QFileDialog, QScrollArea, QSizePolicy, QVBoxLayout, QWidget,
)


class BasePage(QWidget):
    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self._busy = False
        self.settings = QSettings("MORUA", "EBFStudio")

        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)

        self.scroll = QScrollArea()
        scroll = self.scroll
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QScrollArea.NoFrame)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)

        canvas = QWidget()
        canvas.setObjectName("pageCanvas")
        self.column = QVBoxLayout(canvas)
        self.column.setContentsMargins(30, 22, 30, 22)
        self.column.setSpacing(14)

        scroll.setWidget(canvas)
        outer.addWidget(scroll)

        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)

    # ---------- 状态 ----------

    def is_busy(self) -> bool:
        return self._busy

    def scroll_to_bottom(self) -> None:
        """转换开始时把日志拉到眼前，别让用户自己找滚动条。"""
        bar = self.scroll.verticalScrollBar()
        bar.setValue(bar.maximum())

    def request_stop(self) -> None:
        """子类覆盖"""

    # ---------- 文件对话框 ----------

    def choose_files(self, title: str, file_filter: str) -> list[str]:
        names, _ = QFileDialog.getOpenFileNames(self, title, "", file_filter)
        return names

    def choose_dir(self, title: str) -> str:
        return QFileDialog.getExistingDirectory(self, title, "")

    # ---------- 设置 ----------

    def save_bool(self, key: str, value: bool) -> None:
        self.settings.setValue(key, value)

    def load_bool(self, key: str, default: bool = False) -> bool:
        value = self.settings.value(key, default)
        if isinstance(value, str):
            return value.lower() in ("true", "1", "yes")
        return bool(value)

    def load_int(self, key: str, default: int = 0) -> int:
        try:
            return int(self.settings.value(key, default))
        except (TypeError, ValueError):
            return default
