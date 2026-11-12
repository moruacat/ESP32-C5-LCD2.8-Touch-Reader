# -*- coding: utf-8 -*-
"""CI 固件助手入口。

运行：
    python tools\\ci_panel\\main.py

GUI 只负责查询构建、下载 GitHub Actions firmware artifact 和烧录；
不会自动提交代码，也不会自动执行烧录。
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

from app import __version__, theme                                # noqa: E402
from app.window import MainWindow                                # noqa: E402


def _set_app_id() -> None:
    if sys.platform != "win32":
        return
    try:
        import ctypes
        ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID(
            "MORUA.CIFirmwareHelper.1")
    except Exception:
        pass


def _install_excepthook() -> None:
    def hook(exc_type, exc_value, exc_tb) -> None:
        text = "".join(traceback.format_exception(exc_type, exc_value, exc_tb))
        print(text, file=sys.stderr)
        try:
            box = QMessageBox()
            box.setWindowTitle("CI 固件助手出错了")
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
    app.setApplicationName("CI Firmware Helper")
    app.setApplicationDisplayName("CI 固件助手")
    app.setOrganizationName("MORUA")
    app.setApplicationVersion(__version__)
    app.setWindowIcon(theme.app_icon(128))

    theme.apply_theme(app)
    _install_excepthook()

    window = MainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
