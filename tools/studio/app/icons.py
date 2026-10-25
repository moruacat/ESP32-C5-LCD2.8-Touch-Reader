# -*- coding: utf-8 -*-
"""内联 SVG 图标。

不依赖外部图片资源，换颜色只改参数；按设备像素比渲染，缩放不发虚。
所有图标统一 24x24 视角、1.7px 描边，保持线条粗细一致。
"""

from __future__ import annotations

from PySide6.QtCore import QByteArray, QRectF, Qt
from PySide6.QtGui import (
    QColor, QIcon, QLinearGradient, QPainter, QPen, QPixmap,
)
from PySide6.QtSvg import QSvgRenderer

# 24x24 线稿路径
_PATHS = {
    "book": '<path d="M4 19.5A2.5 2.5 0 0 1 6.5 17H20"/>'
            '<path d="M6.5 2H20v20H6.5A2.5 2.5 0 0 1 4 19.5v-15A2.5 2.5 0 0 1 6.5 2z"/>',
    "image": '<rect x="3" y="3" width="18" height="18" rx="3"/>'
             '<circle cx="8.5" cy="8.5" r="1.5"/>'
             '<path d="m21 15-5-5L5 21"/>',
    "info": '<circle cx="12" cy="12" r="9"/>'
            '<path d="M12 11v5"/><path d="M12 7.6h.01"/>',
    "folder": '<path d="M3 7a2 2 0 0 1 2-2h3.6l2 2H19a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>',
    "file": '<path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/>'
            '<path d="M14 2v6h6"/>',
    "upload": '<path d="M12 16V4"/><path d="m6 10 6-6 6 6"/><path d="M4 20h16"/>',
    "check": '<path d="m5 13 4 4L19 7"/>',
    "x": '<path d="M6 6l12 12M18 6 6 18"/>',
    "external": '<path d="M14 4h6v6"/><path d="M20 4 10 14"/>'
                '<path d="M19 14v5a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V6a1 1 0 0 1 1-1h5"/>',
    "trash": '<path d="M4 7h16"/><path d="M9 7V5a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2"/>'
             '<path d="M6 7l1 13h10l1-13"/>',
    "chevron": '<path d="m9 6 6 6-6 6"/>',
    "play": '<path d="M8 5.2v13.6L19.2 12z" fill="{color}" stroke="none"/>',
    "stop": '<rect x="6.5" y="6.5" width="11" height="11" rx="2.5" fill="{color}" stroke="none"/>',
    "bookmark": '<path d="M6 3h12v18l-6-4.5L6 21z"/>',
    "spark": '<path d="M12 3v4"/><path d="M12 17v4"/>'
             '<path d="M3 12h4"/><path d="M17 12h4"/>'
             '<path d="m5.6 5.6 2.8 2.8"/><path d="m15.6 15.6 2.8 2.8"/>'
             '<path d="m18.4 5.6-2.8 2.8"/><path d="m8.4 15.6-2.8 2.8"/>',
}

_cache: dict[tuple[str, str, int, float], QPixmap] = {}


def _svg(name: str, color: str, width: float) -> str:
    body = _PATHS[name].replace("{color}", color)
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" '
        f'fill="none" stroke="{color}" stroke-width="{width}" '
        f'stroke-linecap="round" stroke-linejoin="round">{body}</svg>'
    )


def pixmap(name: str, color: str = "#8A94A6", size: int = 20,
           stroke: float = 1.7, dpr: float = 2.0) -> QPixmap:
    key = (name, color, size, stroke)
    cached = _cache.get(key)
    if cached is not None:
        return cached

    pm = QPixmap(int(size * dpr), int(size * dpr))
    pm.setDevicePixelRatio(dpr)
    pm.fill(Qt.transparent)

    renderer = QSvgRenderer(QByteArray(_svg(name, color, stroke).encode("utf-8")))
    painter = QPainter(pm)
    painter.setRenderHint(QPainter.Antialiasing, True)
    renderer.render(painter, QRectF(0, 0, size, size))
    painter.end()

    _cache[key] = pm
    return pm


def icon(name: str, color: str = "#8A94A6", size: int = 20,
         stroke: float = 1.7) -> QIcon:
    return QIcon(pixmap(name, color, size, stroke))


def app_icon(size: int = 256) -> QIcon:
    """任务栏/窗口图标：深色圆角方块 + 蓝紫渐变书页。"""
    pm = QPixmap(size, size)
    pm.fill(Qt.transparent)

    painter = QPainter(pm)
    painter.setRenderHint(QPainter.Antialiasing, True)

    grad = QLinearGradient(0, 0, size, size)
    grad.setColorAt(0.0, QColor("#101728"))
    grad.setColorAt(1.0, QColor("#0A0D14"))
    painter.setBrush(grad)
    painter.setPen(QPen(QColor("#2A3550"), max(1.0, size / 128)))
    radius = size * 0.24
    painter.drawRoundedRect(QRectF(1, 1, size - 2, size - 2), radius, radius)

    # 书页：两片渐变圆角矩形，中间留书脊
    accent = QLinearGradient(0, 0, size, size)
    accent.setColorAt(0.0, QColor("#6C8CFF"))
    accent.setColorAt(1.0, QColor("#9B7BFF"))
    painter.setPen(Qt.NoPen)
    painter.setBrush(accent)

    m = size * 0.24
    w = (size - 2 * m) / 2 - size * 0.035
    h = size * 0.44
    r = size * 0.07
    painter.drawRoundedRect(QRectF(m, size * 0.30, w, h), r, r)
    painter.drawRoundedRect(QRectF(m + w + size * 0.07, size * 0.30, w, h), r, r)

    painter.end()
    return QIcon(pm)
