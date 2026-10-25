# -*- coding: utf-8 -*-
"""EBF Studio 的配色、字体与全局样式表。

只做一件事：把深色主题一次性套到 QApplication 上。控件自己需要特殊
底色的，用 objectName / 动态属性去命中下面这段 QSS。
"""

from __future__ import annotations

from PySide6.QtGui import QColor, QFont, QPalette
from PySide6.QtWidgets import QApplication

# ---------------- 调色板 ----------------

BG = "#0B0E14"           # 主背景
SIDEBAR = "#080A0F"      # 侧边栏
SURFACE = "#12161F"      # 卡片
SURFACE_2 = "#0C1017"    # 输入框 / 日志
BORDER = "#1E2532"
BORDER_HI = "#2C3648"
TEXT = "#E9EDF5"
MUTED = "#8A94A6"
FAINT = "#5D6779"
ACCENT = "#6C8CFF"
ACCENT_2 = "#9B7BFF"
OK = "#3ECF8E"
WARN = "#E5B567"
ERR = "#FF6B6B"

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

QMainWindow, QWidget#root {{
    background: {BG};
}}

QWidget#content {{
    background: {BG};
}}

QFrame#sidebar {{
    background: {SIDEBAR};
    border-right: 1px solid {BORDER};
}}

QScrollArea, QScrollArea > QWidget > QWidget {{
    background: transparent;
    border: none;
}}

QWidget#pageCanvas {{
    background: transparent;
}}

/* ---------- 文字 ---------- */

QLabel {{
    background: transparent;
}}

QLabel#h1 {{
    font-size: 22px;
    font-weight: 700;
    color: #F4F7FC;
}}

QLabel#subtitle {{
    font-size: 12px;
    color: {MUTED};
}}

QLabel#cardTitle {{
    font-size: 13px;
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

QLabel#success {{
    color: {OK};
}}

QLabel#danger {{
    color: {ERR};
}}

QLabel#brand {{
    font-size: 15px;
    font-weight: 700;
    color: #F4F7FC;
}}

QLabel#brandSub {{
    font-size: 10px;
    color: {FAINT};
}}

QLabel#sectionLabel {{
    font-size: 10px;
    font-weight: 600;
    color: {FAINT};
    padding-left: 4px;
}}

/* ---------- 卡片 ---------- */

QFrame#card {{
    background: {SURFACE};
    border: 1px solid {BORDER};
    border-radius: 12px;
}}

QFrame#divider {{
    background: {BORDER};
    border: none;
    max-height: 1px;
    min-height: 1px;
}}

QFrame#selectionBar {{
    background: #101722;
    border: 1px solid #223047;
    border-radius: 10px;
}}

/* ---------- 按钮 ---------- */

QPushButton {{
    background: #161C27;
    border: 1px solid {BORDER};
    border-radius: 8px;
    padding: 7px 14px;
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
                stop:0 #5B7CFA, stop:1 #8C6DF6);
    border: 1px solid #7B96FF;
    color: #FFFFFF;
    font-weight: 600;
    padding: 10px 22px;
    border-radius: 9px;
}}

QPushButton[variant="primary"]:hover {{
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                stop:0 #6C8BFF, stop:1 #9C7DFF);
}}

QPushButton[variant="primary"]:pressed {{
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                stop:0 #4E6DE6, stop:1 #7C5FE0);
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
    padding: 7px 10px;
}}

QPushButton[variant="ghost"]:hover {{
    background: #131926;
    color: {TEXT};
}}

QPushButton[variant="danger"] {{
    background: transparent;
    border: 1px solid transparent;
    color: {ERR};
    padding: 7px 10px;
}}

QPushButton[variant="danger"]:hover {{
    background: rgba(255, 107, 107, 0.10);
}}

QPushButton#stepBtn {{
    background: transparent;
    border: none;
    border-radius: 6px;
    color: {MUTED};
    font-size: 15px;
    padding: 0;
}}

QPushButton#stepBtn:hover {{
    background: #1A2231;
    color: {TEXT};
}}

QPushButton#stepBtn:disabled {{
    color: #3F4859;
    background: transparent;
}}

QFrame#stepper {{
    background: {SURFACE_2};
    border: 1px solid {BORDER};
    border-radius: 8px;
}}

/* ---------- 侧边栏导航 ---------- */

QToolButton#nav {{
    background: transparent;
    border: none;
    border-radius: 9px;
    color: #97A1B4;
    font-size: 13px;
    padding: 10px 12px;
    text-align: left;
}}

QToolButton#nav:hover {{
    background: #10151E;
    color: #D9E0EC;
}}

QToolButton#nav:checked {{
    background: #151D2E;
    color: #FFFFFF;
    font-weight: 600;
}}

/* ---------- 输入框 ---------- */

QLineEdit {{
    background: {SURFACE_2};
    border: 1px solid {BORDER};
    border-radius: 8px;
    padding: 8px 12px;
    color: {TEXT};
    selection-background-color: #33436B;
}}

QLineEdit:focus {{
    border-color: {ACCENT};
    background: #0D131D;
}}

QLineEdit:disabled {{
    color: {FAINT};
}}

QLineEdit::placeholder {{
    color: {FAINT};
}}

/* ---------- 日志 ---------- */

QTextEdit#log {{
    background: #0A0E15;
    border: 1px solid {BORDER};
    border-radius: 10px;
    padding: 10px 12px;
    color: #C9D2E1;
    selection-background-color: #33436B;
}}

QTextEdit#log:focus {{
    border-color: #26314A;
}}

/* ---------- 进度条 ---------- */

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
                stop:0 #5B7CFA, stop:1 #A07BFF);
}}

/* ---------- 滚动条 ---------- */

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

/* ---------- 其他 ---------- */

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

QFileDialog {{
    background: {BG};
}}
"""


def apply_theme(app: QApplication) -> None:
    """在创建任何窗口之前调用：统一字体、调色板和样式表。"""
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
    palette.setColor(QPalette.Disabled, QPalette.Text, QColor(FAINT))
    palette.setColor(QPalette.Disabled, QPalette.ButtonText, QColor(FAINT))
    app.setPalette(palette)

    app.setStyleSheet(STYLESHEET)
