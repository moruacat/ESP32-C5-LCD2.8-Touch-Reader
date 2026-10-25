# -*- coding: utf-8 -*-
"""通用控件：卡片、拖拽区、开关、步进器、日志、结果条、导航按钮。"""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import (
    Property, QEasingCurve, QPropertyAnimation, QRectF, QSize, Qt, QUrl, Signal,
)
from PySide6.QtGui import (
    QColor, QDesktopServices, QDragEnterEvent, QDropEvent, QFont, QImage,
    QImageReader, QPainter, QPen, QTextCharFormat, QTextCursor, QTransform,
)
from PySide6.QtWidgets import (
    QAbstractButton, QFrame, QHBoxLayout, QLabel, QProgressBar, QPushButton,
    QSizePolicy, QTextEdit, QToolButton, QVBoxLayout, QWidget,
)

from . import icons, theme

IMAGE_SUFFIXES = {".png", ".jpg", ".jpeg", ".webp"}
EPUB_SUFFIXES = {".epub"}


# ---------------- 小工具 ----------------

def human_size(num: float) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if num < 1024 or unit == "GB":
            return f"{num:.0f} {unit}" if unit == "B" else f"{num:.1f} {unit}"
        num /= 1024
    return f"{num:.1f} GB"


def repolish(widget: QWidget) -> None:
    widget.style().unpolish(widget)
    widget.style().polish(widget)
    widget.update()


def label(text: str, kind: str = "", wrap: bool = False) -> QLabel:
    lb = QLabel()
    lb.setTextFormat(Qt.PlainText)      # 描述里会有 <h1> 这类文字，别让 QLabel 当 HTML 解析
    lb.setText(text)
    if kind:
        lb.setObjectName(kind)
    if wrap:
        lb.setWordWrap(True)
    return lb


# ---------------- 开关 ----------------

class Switch(QAbstractButton):
    """iOS 风格滑动开关，带 150ms 动画。"""

    def __init__(self, checked: bool = False, parent: QWidget | None = None):
        super().__init__(parent)
        self.setCheckable(True)
        self.setChecked(checked)
        self.setCursor(Qt.PointingHandCursor)
        self.setFixedSize(42, 24)
        self.setFocusPolicy(Qt.StrongFocus)

        self._offset = 21.0 if checked else 3.0
        self._anim = QPropertyAnimation(self, b"offset", self)
        self._anim.setDuration(150)
        self._anim.setEasingCurve(QEasingCurve.OutCubic)
        self.toggled.connect(self._animate)

    def _get_offset(self) -> float:
        return self._offset

    def _set_offset(self, value: float) -> None:
        self._offset = value
        self.update()

    offset = Property(float, _get_offset, _set_offset)

    def _animate(self, on: bool) -> None:
        self._anim.stop()
        self._anim.setStartValue(self._offset)
        self._anim.setEndValue(21.0 if on else 3.0)
        self._anim.start()

    def paintEvent(self, _event) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing, True)

        enabled = self.isEnabled()
        if not enabled:
            track = QColor("#1B2230")
        elif self.isChecked():
            track = QColor(theme.ACCENT)
        else:
            track = QColor("#2A3345")

        painter.setPen(Qt.NoPen)
        painter.setBrush(track)
        painter.drawRoundedRect(QRectF(0, 0, 42, 24), 12, 12)

        knob = QColor("#FFFFFF") if enabled else QColor("#7A8496")
        if not self.isChecked() and enabled:
            knob = QColor("#C3CBDA")
        painter.setBrush(knob)
        painter.drawEllipse(QRectF(self._offset, 3, 18, 18))

        if self.hasFocus():
            painter.setBrush(Qt.NoBrush)
            painter.setPen(QPen(QColor(theme.ACCENT), 1))
            painter.drawRoundedRect(QRectF(1, 1, 40, 22), 11, 11)

    def sizeHint(self) -> QSize:
        return QSize(42, 24)


# ---------------- 数字步进器 ----------------

class NumberStepper(QFrame):
    valueChanged = Signal(int)

    def __init__(self, minimum: int = 0, maximum: int = 99, value: int = 0,
                 parent: QWidget | None = None):
        super().__init__(parent)
        self.setObjectName("stepper")
        self.setFixedSize(120, 34)

        self._min = minimum
        self._max = maximum
        self._value = max(minimum, min(maximum, value))

        row = QHBoxLayout(self)
        row.setContentsMargins(4, 3, 4, 3)
        row.setSpacing(2)

        self.btn_minus = QPushButton("−")
        self.btn_minus.setObjectName("stepBtn")
        self.btn_minus.setFixedSize(28, 28)
        self.btn_minus.setCursor(Qt.PointingHandCursor)

        self.btn_plus = QPushButton("+")
        self.btn_plus.setObjectName("stepBtn")
        self.btn_plus.setFixedSize(28, 28)
        self.btn_plus.setCursor(Qt.PointingHandCursor)

        self.value_label = QLabel(str(self._value))
        self.value_label.setAlignment(Qt.AlignCenter)
        self.value_label.setFont(QFont(theme.MONO_FONT, 10))
        self.value_label.setStyleSheet(f"color: {theme.TEXT}; background: transparent;")

        row.addWidget(self.btn_minus)
        row.addWidget(self.value_label, 1)
        row.addWidget(self.btn_plus)

        self.btn_minus.clicked.connect(lambda: self.setValue(self._value - 1))
        self.btn_plus.clicked.connect(lambda: self.setValue(self._value + 1))
        self._sync_buttons()

    def value(self) -> int:
        return self._value

    def setValue(self, value: int) -> None:
        value = max(self._min, min(self._max, int(value)))
        if value == self._value:
            return
        self._value = value
        self.value_label.setText(str(value))
        self._sync_buttons()
        self.valueChanged.emit(value)

    def _sync_buttons(self) -> None:
        self.btn_minus.setEnabled(self._value > self._min)
        self.btn_plus.setEnabled(self._value < self._max)


# ---------------- 卡片 ----------------

class Card(QFrame):
    """圆角深色卡片，标题行右侧可继续放控件。"""

    def __init__(self, title: str = "", subtitle: str = "",
                 parent: QWidget | None = None):
        super().__init__(parent)
        self.setObjectName("card")

        self.body = QVBoxLayout(self)
        self.body.setContentsMargins(16, 14, 16, 14)
        self.body.setSpacing(10)

        self.header = QHBoxLayout()
        self.header.setSpacing(10)
        if title:
            texts = QVBoxLayout()
            texts.setSpacing(2)
            texts.addWidget(label(title, "cardTitle"))
            if subtitle:
                texts.addWidget(label(subtitle, "caption", wrap=True))
            self.header.addLayout(texts, 1)
            self.body.addLayout(self.header)

    def add(self, widget: QWidget, stretch: int = 0) -> None:
        self.body.addWidget(widget, stretch)

    def add_layout(self, layout, stretch: int = 0) -> None:
        self.body.addLayout(layout, stretch)


class Divider(QFrame):
    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.setObjectName("divider")
        self.setFixedHeight(1)


# ---------------- 拖拽选择区 ----------------

class DropZone(QFrame):
    """虚线框：可点击选择、可拖入文件/文件夹。"""

    browseFiles = Signal()
    browseFolder = Signal()
    pathsDropped = Signal(list)

    def __init__(self, icon_name: str, title: str, subtitle: str, hint: str,
                 suffixes: set[str], parent: QWidget | None = None):
        super().__init__(parent)
        self._suffixes = {s.lower() for s in suffixes}
        self._hover = False
        self._drag = False

        self.setAcceptDrops(True)
        self.setCursor(Qt.PointingHandCursor)
        self.setMinimumHeight(158)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)

        box = QVBoxLayout(self)
        box.setContentsMargins(22, 14, 22, 12)
        box.setSpacing(3)
        box.setAlignment(Qt.AlignCenter)

        icon_label = QLabel()
        icon_label.setPixmap(icons.pixmap(icon_name, theme.ACCENT, 34, 1.6))
        icon_label.setAlignment(Qt.AlignCenter)
        icon_label.setFixedHeight(42)

        self.title_label = QLabel(title)
        self.title_label.setTextFormat(Qt.PlainText)
        self.title_label.setAlignment(Qt.AlignCenter)
        self.title_label.setStyleSheet(
            f"color: {theme.TEXT}; font-size: 14px; font-weight: 600; background: transparent;")

        self.sub_label = QLabel(subtitle)
        self.sub_label.setTextFormat(Qt.PlainText)
        self.sub_label.setAlignment(Qt.AlignCenter)
        self.sub_label.setStyleSheet(
            f"color: {theme.MUTED}; font-size: 12px; background: transparent;")

        self.hint_label = QLabel(hint)
        self.hint_label.setTextFormat(Qt.PlainText)
        self.hint_label.setAlignment(Qt.AlignCenter)
        self.hint_label.setStyleSheet(
            f"color: {theme.FAINT}; font-size: 11px; background: transparent;")

        buttons = QHBoxLayout()
        buttons.setSpacing(6)
        buttons.addStretch(1)
        btn_file = QPushButton("选择文件")
        btn_file.setProperty("variant", "ghost")
        btn_file.setCursor(Qt.PointingHandCursor)
        btn_file.clicked.connect(self.browseFiles.emit)
        btn_dir = QPushButton("选择文件夹")
        btn_dir.setProperty("variant", "ghost")
        btn_dir.setCursor(Qt.PointingHandCursor)
        btn_dir.clicked.connect(self.browseFolder.emit)
        buttons.addWidget(btn_file)
        buttons.addWidget(btn_dir)
        buttons.addStretch(1)

        box.addWidget(icon_label)
        box.addWidget(self.title_label)
        box.addWidget(self.sub_label)
        box.addWidget(self.hint_label)
        box.addSpacing(4)
        box.addLayout(buttons)

    # ---------- 绘制 ----------

    def paintEvent(self, _event) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing, True)

        rect = QRectF(self.rect()).adjusted(1, 1, -1, -1)
        if self._drag:
            fill = QColor(108, 140, 255, 26)
            border = QColor(theme.ACCENT)
        elif self._hover:
            fill = QColor("#111823")
            border = QColor("#39445A")
        else:
            fill = QColor("#0E121A")
            border = QColor("#293244")

        painter.setBrush(fill)
        pen = QPen(border, 1.4)
        pen.setStyle(Qt.DashLine)
        pen.setDashPattern([5, 4])
        painter.setPen(pen)
        painter.drawRoundedRect(rect, 14, 14)

    def enterEvent(self, event) -> None:
        self._hover = True
        self.update()
        super().enterEvent(event)

    def leaveEvent(self, event) -> None:
        self._hover = False
        self.update()
        super().leaveEvent(event)

    def mouseReleaseEvent(self, event) -> None:
        if event.button() == Qt.LeftButton and self.rect().contains(event.position().toPoint()):
            self.browseFiles.emit()
        super().mouseReleaseEvent(event)

    # ---------- 拖拽 ----------

    def _accepted(self, urls: list[QUrl]) -> bool:
        for url in urls:
            path = Path(url.toLocalFile())
            if path.is_dir():
                return True
            if path.suffix.lower() in self._suffixes:
                return True
        return False

    def dragEnterEvent(self, event: QDragEnterEvent) -> None:
        if self._accepted(event.mimeData().urls()):
            self._drag = True
            self.update()
            event.acceptProposedAction()
        else:
            event.ignore()

    def dragLeaveEvent(self, event) -> None:
        self._drag = False
        self.update()
        super().dragLeaveEvent(event)

    def dropEvent(self, event: QDropEvent) -> None:
        self._drag = False
        self.update()
        paths = [u.toLocalFile() for u in event.mimeData().urls() if u.isLocalFile()]
        if paths:
            self.pathsDropped.emit(paths)
            event.acceptProposedAction()


class SelectionBar(QFrame):
    """显示当前选中了什么，右侧可一键清空。"""

    cleared = Signal()

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.setObjectName("selectionBar")

        row = QHBoxLayout(self)
        row.setContentsMargins(12, 8, 8, 8)
        row.setSpacing(10)

        self.icon_label = QLabel()
        self.icon_label.setFixedSize(22, 22)
        self.icon_label.setAlignment(Qt.AlignCenter)

        self.title_label = QLabel()
        self.title_label.setTextFormat(Qt.PlainText)
        self.title_label.setStyleSheet(
            f"color: {theme.TEXT}; font-size: 12px; font-weight: 600; background: transparent;")
        self.sub_label = QLabel()
        self.sub_label.setTextFormat(Qt.PlainText)
        self.sub_label.setStyleSheet(
            f"color: {theme.FAINT}; font-size: 11px; background: transparent;")

        texts = QVBoxLayout()
        texts.setSpacing(1)
        texts.addWidget(self.title_label)
        texts.addWidget(self.sub_label)

        self.clear_btn = QPushButton("清空")
        self.clear_btn.setProperty("variant", "ghost")
        self.clear_btn.setCursor(Qt.PointingHandCursor)
        self.clear_btn.setFixedHeight(28)
        self.clear_btn.clicked.connect(self.cleared.emit)

        row.addWidget(self.icon_label)
        row.addLayout(texts, 1)
        row.addWidget(self.clear_btn)
        self.setVisible(False)

    def update_paths(self, paths: list[str]) -> None:
        paths = [p for p in paths if p]
        if not paths:
            self.setVisible(False)
            return

        if len(paths) == 1:
            path = Path(paths[0])
            if path.is_dir():
                self.icon_label.setPixmap(icons.pixmap("folder", theme.ACCENT, 18))
                self.title_label.setText(path.name or str(path))
                self.sub_label.setText("文件夹 · 递归扫描全部子目录")
            else:
                self.icon_label.setPixmap(icons.pixmap("file", theme.ACCENT, 18))
                try:
                    size = human_size(path.stat().st_size)
                except OSError:
                    size = "未知大小"
                self.title_label.setText(path.name)
                self.sub_label.setText(f"{path.parent}  ·  {size}")
        else:
            files = 0
            total = 0
            for raw in paths:
                path = Path(raw)
                if path.is_file():
                    files += 1
                    try:
                        total += path.stat().st_size
                    except OSError:
                        pass
            self.icon_label.setPixmap(icons.pixmap("folder", theme.ACCENT, 18))
            self.title_label.setText(f"已选择 {len(paths)} 个项目")
            extra = f"其中 {files} 个文件 · {human_size(total)}" if files else "多个文件夹"
            self.sub_label.setText(extra)

        self.setVisible(True)


# ---------------- 日志 ----------------

_LOG_COLORS = {
    "info": "#C5CEE0",
    "muted": "#7B8699",
    "ok": theme.OK,
    "warn": theme.WARN,
    "error": theme.ERR,
    "info_accent": "#8FA8FF",
}


class LogView(QTextEdit):
    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.setObjectName("log")
        self.setReadOnly(True)
        self.setAcceptRichText(False)
        self.setLineWrapMode(QTextEdit.WidgetWidth)
        self.document().setMaximumBlockCount(4000)
        self.setFont(QFont(theme.MONO_FONT, 9))
        self.setMinimumHeight(190)
        self.setTextInteractionFlags(Qt.TextSelectableByMouse | Qt.TextSelectableByKeyboard)

    def append_line(self, text: str, level: str = "info") -> None:
        fmt = QTextCharFormat()
        fmt.setForeground(QColor(_LOG_COLORS.get(level, _LOG_COLORS["info"])))
        cursor = self.textCursor()
        cursor.movePosition(QTextCursor.End)
        cursor.insertText(text + "\n", fmt)
        self.setTextCursor(cursor)
        self.ensureCursorVisible()

    def reset(self) -> None:
        self.clear()


# ---------------- 结果提示条 ----------------

class ResultBanner(QFrame):
    _STYLES = {
        "success": (theme.OK, "rgba(62, 207, 142, 0.09)", "check"),
        "warn": (theme.WARN, "rgba(229, 181, 103, 0.09)", "info"),
        "error": (theme.ERR, "rgba(255, 107, 107, 0.09)", "x"),
    }

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.setObjectName("banner")
        self.setVisible(False)

        row = QHBoxLayout(self)
        row.setContentsMargins(12, 10, 10, 10)
        row.setSpacing(10)

        self.icon_label = QLabel()
        self.icon_label.setFixedSize(20, 20)

        self.title_label = QLabel()
        self.title_label.setTextFormat(Qt.PlainText)
        self.title_label.setStyleSheet("font-size: 12px; font-weight: 600; background: transparent;")

        self.detail_label = QLabel()
        self.detail_label.setTextFormat(Qt.PlainText)
        self.detail_label.setStyleSheet(
            f"color: {theme.MUTED}; font-size: 11px; background: transparent;")

        texts = QVBoxLayout()
        texts.setSpacing(1)
        texts.addWidget(self.title_label)
        texts.addWidget(self.detail_label)

        self.open_btn = QPushButton("打开目录")
        self.open_btn.setProperty("variant", "ghost")
        self.open_btn.setCursor(Qt.PointingHandCursor)
        self.open_btn.setFixedHeight(28)
        self._open_slot = None

        row.addWidget(self.icon_label)
        row.addLayout(texts, 1)
        row.addWidget(self.open_btn)

    def show_result(self, kind: str, title: str, detail: str = "",
                    path: str | None = None) -> None:
        color, background, icon_name = self._STYLES.get(kind, self._STYLES["warn"])
        self.icon_label.setPixmap(icons.pixmap(icon_name, color, 17, 2.0))
        self.title_label.setText(title)
        self.title_label.setStyleSheet(
            f"color: {color}; font-size: 12px; font-weight: 600; background: transparent;")
        self.detail_label.setText(detail)
        self.detail_label.setVisible(bool(detail))
        self.setStyleSheet(
            f"QFrame#banner {{ background: {background}; border: 1px solid {color};"
            f" border-radius: 10px; }}")

        if self._open_slot is not None:
            try:
                self.open_btn.clicked.disconnect(self._open_slot)
            except (RuntimeError, TypeError):
                pass
            self._open_slot = None

        if path:
            target = Path(path)
            if target.is_file():
                target = target.parent
            self.open_btn.setVisible(True)
            self._open_slot = lambda _=False, p=target: QDesktopServices.openUrl(
                QUrl.fromLocalFile(str(p)))
            self.open_btn.clicked.connect(self._open_slot)
        else:
            self.open_btn.setVisible(False)

        self.setVisible(True)


# ---------------- 导航 ----------------

class NavButton(QToolButton):
    def __init__(self, icon_name: str, text: str, parent: QWidget | None = None):
        super().__init__(parent)
        self._icon_name = icon_name
        self.setObjectName("nav")
        self.setText("   " + text)
        self.setCheckable(True)
        self.setAutoExclusive(True)
        self.setToolButtonStyle(Qt.ToolButtonTextBesideIcon)
        self.setIconSize(QSize(18, 18))
        self.setCursor(Qt.PointingHandCursor)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        self.setMinimumHeight(40)
        self.toggled.connect(self._sync_icon)
        self._sync_icon(False)

    def _sync_icon(self, active: bool) -> None:
        color = "#FFFFFF" if active else "#97A1B4"
        self.setIcon(icons.icon(self._icon_name, color, 18))


# ---------------- 页面标题 ----------------

class PageHeader(QWidget):
    def __init__(self, title: str, subtitle: str, parent: QWidget | None = None):
        super().__init__(parent)
        row = QHBoxLayout(self)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(16)

        texts = QVBoxLayout()
        texts.setSpacing(5)
        texts.addWidget(label(title, "h1"))
        texts.addWidget(label(subtitle, "subtitle", wrap=True))
        row.addLayout(texts, 1)

        self.actions = QHBoxLayout()
        self.actions.setSpacing(8)
        row.addLayout(self.actions, 0)

    def add_action(self, widget: QWidget) -> None:
        self.actions.addWidget(widget)


def setting_row(title: str, desc: str, control: QWidget) -> QWidget:
    """左侧标题+说明，右侧控件的设置行。"""
    wrapper = QWidget()
    row = QHBoxLayout(wrapper)
    row.setContentsMargins(0, 2, 0, 2)
    row.setSpacing(20)

    texts = QVBoxLayout()
    texts.setSpacing(2)
    texts.addWidget(label(title))
    if desc:
        texts.addWidget(label(desc, "caption", wrap=True))

    row.addLayout(texts, 1)
    row.addWidget(control, 0, Qt.AlignVCenter)
    return wrapper


# ---------------- 进度条 ----------------

class ProgressBar(QProgressBar):
    """细长圆角进度条，样式全部来自 QSS。"""

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.setTextVisible(False)
        self.setRange(0, 100)
        self.setValue(0)
        self.setFixedHeight(6)


# ---------------- 图片预览 ----------------

def _preview_fit_area(w: int, h: int) -> float:
    scale = min(320 / w, 240 / h, 1.0)
    return w * scale * h * scale


def _preview_should_rotate(w: int, h: int) -> bool:
    """与 photo2raw.should_rotate_ccw 保持同一套规则（预览不直接依赖转换器模块）。"""
    if w <= 0 or h <= 0:
        return False
    return _preview_fit_area(h, w) > _preview_fit_area(w, h) + 0.5


class ImagePreview(QFrame):
    """按输出规格（320x240 黑底居中）预览第一张图。"""

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.setObjectName("card")
        self.setMinimumHeight(230)
        self.setMinimumWidth(240)

        self._image: QImage | None = None
        self._source_size: QSize | None = None
        self._name = ""
        self._rotated = False

        box = QVBoxLayout(self)
        box.setContentsMargins(16, 14, 16, 12)
        box.setSpacing(8)

        head = QHBoxLayout()
        head.addWidget(label("输出预览", "cardTitle"))
        head.addStretch(1)
        self.badge = label("320 × 240 · RGB565", "caption")
        head.addWidget(self.badge)
        box.addLayout(head)

        self.canvas = _PreviewCanvas(self)
        box.addWidget(self.canvas, 1)

        self.foot = label("选中图片后在这里预览", "caption")
        self.foot.setAlignment(Qt.AlignCenter)
        box.addWidget(self.foot)

    def set_paths(self, paths: list[str], suffixes: set[str],
                  auto_rotate: bool = True) -> None:
        target: Path | None = None
        for raw in paths:
            path = Path(raw)
            if path.is_file() and path.suffix.lower() in suffixes:
                target = path
                break
            if path.is_dir():
                for child in sorted(path.iterdir()):
                    if child.is_file() and child.suffix.lower() in suffixes:
                        target = child
                        break
            if target is not None:
                break

        if target is None:
            self.clear()
            return

        reader = QImageReader(str(target))
        # photo2raw 用 PIL 直接解码，不应用 EXIF 方向；预览也保持一致，避免看到的方向和输出不同
        reader.setAutoTransform(False)
        source = reader.size()
        if not source.isValid():
            self.clear()
            return

        rotate = auto_rotate and _preview_should_rotate(source.width(), source.height())
        # 按旋转后的目标尺寸解码，避免先缩到小尺寸再旋转导致预览偏糊
        effective = QSize(source.height(), source.width()) if rotate else source
        scaled = effective.scaled(QSize(320, 240), Qt.KeepAspectRatio)
        reader.setScaledSize(QSize(scaled.height(), scaled.width()) if rotate else scaled)
        image = reader.read()
        if image.isNull():
            self.clear()
            return
        if rotate:
            image = image.transformed(QTransform().rotate(-90))

        self._image = image
        self._source_size = source
        self._name = target.name
        self._rotated = rotate
        self.canvas.set_image(image)
        turned = "  ·  逆时针 90°" if rotate else ""
        self.foot.setText(
            f"{target.name}  ·  原图 {source.width()}×{source.height()}{turned}")
        self.badge.setText("320 × 240 · RGB565")

    def clear(self) -> None:
        self._image = None
        self._source_size = None
        self._rotated = False
        self.canvas.set_image(None)
        self.foot.setText("选中图片后在这里预览")
        self.badge.setText("320 × 240 · RGB565")


class _PreviewCanvas(QWidget):
    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self._image: QImage | None = None
        self.setMinimumHeight(120)

    def set_image(self, image: QImage | None) -> None:
        self._image = image
        self.update()

    def paintEvent(self, _event) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing, True)

        area = QRectF(0, 0, self.width(), self.height())
        # 按真实输出的 4:3 比例画一块黑底画布，预览比例才和成品一致
        width = min(area.width(), area.height() * 4 / 3)
        height = width * 3 / 4
        frame = QRectF(
            area.center().x() - width / 2,
            area.center().y() - height / 2,
            width, height,
        )

        painter.setBrush(QColor("#05070B"))
        painter.setPen(QPen(QColor("#1B2230"), 1))
        painter.drawRoundedRect(frame.adjusted(0.5, 0.5, -0.5, -0.5), 7, 7)

        if self._image is None or self._image.isNull():
            painter.setPen(QColor(theme.FAINT))
            painter.drawText(frame, Qt.AlignCenter, "无预览")
            return

        area = frame.adjusted(5, 5, -5, -5)
        scaled = self._image.size().scaled(area.size().toSize(), Qt.KeepAspectRatio)
        x = area.x() + (area.width() - scaled.width()) / 2
        y = area.y() + (area.height() - scaled.height()) / 2
        painter.drawImage(QRectF(x, y, scaled.width(), scaled.height()), self._image)
