#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""EBF Studio 入口。

运行：
    python tools\\studio\\main.py

依赖：PySide6、Pillow（Pillow 只在处理封面/图片时需要）。
"""

from __future__ import annotations

import sys
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

from PySide6.QtCore import Qt                                    # noqa: E402
from PySide6.QtWidgets import QApplication, QMessageBox          # noqa: E402

from app import __version__, icons                               # noqa: E402
from app.theme import apply_theme                                # noqa: E402
from app.window import MainWindow                                # noqa: E402


def _set_app_id() -> None:
    """让任务栏把窗口归到独立的应用图标下。"""
    if sys.platform != "win32":
        return
    try:
        import ctypes
        ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID(
            "MORUA.EBFStudio.1")
    except Exception:
        pass


def _install_excepthook() -> None:
    def hook(exc_type, exc_value, exc_tb) -> None:
        text = "".join(traceback.format_exception(exc_type, exc_value, exc_tb))
        print(text, file=sys.stderr)
        try:
            box = QMessageBox()
            box.setWindowTitle("EBF Studio 出错了")
            box.setText("程序遇到了未处理的错误")
            box.setInformativeText(str(exc_value))
            box.setDetailedText(text)
            box.setIcon(QMessageBox.Critical)
            box.exec()
        except Exception:
            pass

    sys.excepthook = hook


def main() -> int:
    try:
        QApplication.setHighDpiScaleFactorRoundingPolicy(
            Qt.HighDpiScaleFactorRoundingPolicy.PassThrough)
    except Exception:
        pass

    _set_app_id()

    app = QApplication(sys.argv)
    app.setApplicationName("EBF Studio")
    app.setApplicationDisplayName("EBF Studio")
    app.setOrganizationName("MORUA")
    app.setApplicationVersion(__version__)
    app.setWindowIcon(icons.app_icon(128))

    apply_theme(app)
    _install_excepthook()

    window = MainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
