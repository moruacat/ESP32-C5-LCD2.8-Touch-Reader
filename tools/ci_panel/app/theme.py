# -*- coding: utf-8 -*-
"""CI 固件助手的深色主题与运行时图标。

界面主色沿用项目约定的 #1585C0，控件只做静态深色样式，不加入闪烁和
无意义动画。
"""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtGui import QColor, QFont, QIcon, QPainter, QPalette, QPixmap
from PySide6.QtWidgets import QApplication

# ---------------- 调色板 ----------------

BG = "#0B0E14"
SURFACE = "#12161F"
SURFACE_2 = "#0C1017"
SURFACE_3 = "#171E2A"
BORDER = "#1E2532"
BORDER_HI = "#2C3648"
TEXT = "#E9EDF5"
MUTED = "#8A94A6"
FAINT = "#5D6779"
ACCENT = "#1585C0"
ACCENT_HI = "#23A3E6"
ACCENT_DARK = "#106A9A"
OK = "#3ECF8E"
WARN = "#E5B567"
ERR = "#FF6B6B"
INFO = "#60B8E8"

UI_FONT = "Microsoft YaHei UI"
MONO_FONT = "Cascadia Mono"


STYLESHEET = f"""
* {{
    outline: none;
}}

QWidget {{
    color: {TEXT};
    font-family: "{UI_FONT}", "Segoe UI", sans-serif;
    font-size: 13px;
}}

QMainWindow, QWidget#root, QDialog {{
    background: {BG};
}}

QLabel {{
    background: transparent;
}}

QLabel#h1 {{
    font-size: 23px;
    font-weight: 700;
    color: #F4F7FC;
}}

QLabel#subtitle {{
    font-size: 12px;
    color: {MUTED};
}}

QLabel#cardTitle {{
    font-size: 14px;
    font-weight: 600;
    color: {TEXT};
}}

QLabel#caption {{
    font-size: 11px;
    color: #78849A;
}}

QLabel#muted {{
    color: {MUTED};
}}

QLabel#faint {{
    color: {FAINT};
}}

QLabel#sectionLabel {{
    font-size: 10px;
    font-weight: 600;
    color: {FAINT};
    letter-spacing: 1px;
}}

QLabel#badge {{
    color: {INFO};
    background: #0D2633;
    border: 1px solid #17445A;
    border-radius: 9px;
    padding: 3px 9px;
    font-size: 11px;
}}

/* ---------------- 卡片 ---------------- */

QFrame#card {{
    background: {SURFACE};
    border: 1px solid {BORDER};
    border-radius: 12px;
}}

QFrame#innerCard {{
    background: {SURFACE_2};
    border: 1px solid {BORDER};
    border-radius: 9px;
}}

QFrame#divider {{
    background: {BORDER};
    border: none;
    max-height: 1px;
    min-height: 1px;
}}

/* ---------------- 按钮 ---------------- */

QPushButton {{
    background: #161C27;
    border: 1px solid {BORDER};
    border-radius: 8px;
    padding: 7px 13px;
    color: #D6DDEA;
}}

QPushButton:hover {{
    background: #1B2331;
    border-color: {BORDER_HI};
}}

QPushButton:pressed {{
    background: #11161F;
}}

QPushButton:disabled {{
    color: #566073;
    background: #10141C;
    border-color: #1A202B;
}}

QPushButton[variant="primary"] {{
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                stop:0 {ACCENT}, stop:1 #1C9EDB);
    border: 1px solid {ACCENT_HI};
    color: #FFFFFF;
    font-weight: 600;
}}

QPushButton[variant="primary"]:hover {{
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                stop:0 {ACCENT_HI}, stop:1 #28AEE8);
}}

QPushButton[variant="primary"]:pressed {{
    background: {ACCENT_DARK};
}}

QPushButton[variant="primary"]:disabled {{
    background: #1B2231;
    border-color: #232C3D;
    color: #5B6578;
}}

QPushButton[variant="ghost"] {{
    background: transparent;
    border: 1px solid transparent;
    color: {MUTED};
    padding: 7px 9px;
}}

QPushButton[variant="ghost"]:hover {{
    background: #131926;
    color: {TEXT};
}}

QPushButton[variant="danger"] {{
    background: #2A171B;
    border: 1px solid #5A2B33;
    color: #FF9A9A;
    font-weight: 600;
}}

QPushButton[variant="danger"]:hover {{
    background: #3A1B21;
    border-color: #7A3844;
}}

QPushButton[variant="danger"]:disabled {{
    background: #171215;
    border-color: #2B1A1E;
    color: #66484E;
}}

/* ---------------- 输入 ---------------- */

QLineEdit {{
    background: {SURFACE_2};
    border: 1px solid {BORDER};
    border-radius: 8px;
    padding: 8px 11px;
    color: {TEXT};
    selection-background-color: #164E6B;
}}

QLineEdit:focus {{
    border-color: {ACCENT};
    background: #0D131D;
}}

QLineEdit:disabled {{
    color: {FAINT};
    background: #0A0D13;
}}

QLineEdit::placeholder {{
    color: {FAINT};
}}

QComboBox {{
    background: {SURFACE_2};
    border: 1px solid {BORDER};
    border-radius: 8px;
    padding: 7px 10px;
    color: {TEXT};
    min-height: 18px;
}}

QComboBox:focus, QComboBox:on {{
    border-color: {ACCENT};
}}

QComboBox:disabled {{
    color: {FAINT};
    background: #0A0D13;
}}

QComboBox::drop-down {{
    border: none;
    width: 24px;
}}

QComboBox QAbstractItemView {{
    background: {SURFACE};
    color: {TEXT};
    border: 1px solid {BORDER_HI};
    selection-background-color: #174D68;
    selection-color: #FFFFFF;
    outline: none;
}}

QCheckBox {{
    color: {MUTED};
    spacing: 6px;
}}

QCheckBox:disabled {{
    color: {FAINT};
}}

/* ---------------- 表格 ---------------- */

QTableWidget {{
    background: {SURFACE_2};
    alternate-background-color: #0E131C;
    border: 1px solid {BORDER};
    border-radius: 9px;
    color: {TEXT};
    selection-background-color: #123A50;
    selection-color: #FFFFFF;
    gridline-color: transparent;
}}

QTableWidget::item {{
    padding: 7px 8px;
    border-bottom: 1px solid #141A24;
}}

QTableWidget::item:selected {{
    background: #123A50;
}}

QHeaderView::section {{
    background: #0A0E15;
    color: {FAINT};
    border: none;
    border-bottom: 1px solid {BORDER};
    padding: 8px;
    font-size: 11px;
    font-weight: 600;
}}

QTableCornerButton::section {{
    background: #0A0E15;
    border: none;
}}

/* ---------------- 文本区域 ---------------- */

QTextEdit#log, QPlainTextEdit#logView {{
    background: #0A0E15;
    border: 1px solid {BORDER};
    border-radius: 9px;
    padding: 9px 11px;
    color: #C9D2E1;
    selection-background-color: #164E6B;
    font-family: "{MONO_FONT}", "Consolas", monospace;
    font-size: 12px;
}}

QTextEdit#log:focus, QPlainTextEdit#logView:focus {{
    border-color: #26314A;
}}

/* ---------------- 进度条 ---------------- */

QProgressBar {{
    background: #171E2A;
    border: none;
    border-radius: 3px;
    min-height: 6px;
    max-height: 6px;
    text-align: center;
    color: transparent;
}}

QProgressBar::chunk {{
    border-radius: 3px;
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                stop:0 {ACCENT}, stop:1 #3DB4EB);
}}

/* ---------------- 滚动条 ---------------- */

QScrollBar:vertical {{
    background: transparent;
    width: 10px;
    margin: 3px 2px;
}}

QScrollBar::handle:vertical {{
    background: #273042;
    border-radius: 4px;
    min-height: 32px;
}}

QScrollBar::handle:vertical:hover {{
    background: #38445C;
}}

QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{
    height: 0;
}}

QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {{
    background: transparent;
}}

QScrollBar:horizontal {{
    background: transparent;
    height: 10px;
    margin: 2px 3px;
}}

QScrollBar::handle:horizontal {{
    background: #273042;
    border-radius: 4px;
    min-width: 32px;
}}

QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {{
    width: 0;
}}

QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {{
    background: transparent;
}}

/* ---------------- 其他 ---------------- */

QToolTip {{
    background: #161D29;
    color: {TEXT};
    border: 1px solid {BORDER_HI};
    border-radius: 6px;
    padding: 6px 8px;
}}

QMessageBox {{
    background: {SURFACE};
}}

QMessageBox QLabel {{
    color: {TEXT};
    font-size: 13px;
}}

QMessageBox QPushButton {{
    min-width: 78px;
}}
"""


def apply_theme(app: QApplication) -> None:
    """在创建窗口之前调用，统一调色板、字体与样式表。"""
    app.setStyle("Fusion")

    font = QFont(UI_FONT)
    font.setPointSize(10)
    app.setFont(font)

    palette = QPalette()
    palette.setColor(QPalette.Window, QColor(BG))
    palette.setColor(QPalette.WindowText, QColor(TEXT))
    palette.setColor(QPalette.Base, QColor(SURFACE_2))
    palette.setColor(QPalette.AlternateBase, QColor(SURFACE))
    palette.setColor(QPalette.Text, QColor(TEXT))
    palette.setColor(QPalette.Button, QColor(SURFACE))
    palette.setColor(QPalette.ButtonText, QColor(TEXT))
    palette.setColor(QPalette.Highlight, QColor(ACCENT))
    palette.setColor(QPalette.HighlightedText, QColor("#FFFFFF"))
    palette.setColor(QPalette.ToolTipBase, QColor(SURFACE_2))
    palette.setColor(QPalette.ToolTipText, QColor(TEXT))
    palette.setColor(QPalette.PlaceholderText, QColor(FAINT))
    palette.setColor(QPalette.Link, QColor(INFO))
    palette.setColor(QPalette.Disabled, QPalette.Text, QColor(FAINT))
    palette.setColor(QPalette.Disabled, QPalette.ButtonText, QColor(FAINT))
    app.setPalette(palette)
    app.setStyleSheet(STYLESHEET)


def app_icon(size: int = 64) -> QIcon:
    """画一个简单的「CI」应用图标，避免依赖外部图片资源。"""
    pixmap = QPixmap(size, size)
    pixmap.fill(Qt.transparent)

    painter = QPainter(pixmap)
    painter.setRenderHint(QPainter.Antialiasing, True)
    painter.setPen(Qt.NoPen)
    painter.setBrush(QColor(ACCENT))
    radius = size * 0.22
    painter.drawRoundedRect(2, 2, size - 4, size - 4, radius, radius)

    painter.setPen(QColor("#FFFFFF"))
    font = QFont("Segoe UI", max(10, int(size * 0.33)), QFont.Bold)
    painter.setFont(font)
    painter.drawText(pixmap.rect(), Qt.AlignCenter, "CI")
    painter.end()

    return QIcon(pixmap)
