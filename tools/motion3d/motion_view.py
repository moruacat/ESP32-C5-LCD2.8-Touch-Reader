#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""电脑端实时 3D 姿态显示（配合固件「设置 → 关于 → 姿态广播」）。

数据来自设备通过 UDP 4210 端口广播的文本行：

    MOTION <seq> <t_ms> <qw> <qx> <qy> <qz> <ax> <ay> <az> <gx> <gy> <gz>

    四元数 ×10000，加速度 ×1000（单位 g），角速度 ×100（单位 °/s）

界面：
  - 3D 视图：简化外壳模型（外壳 + 屏幕 + 磁吸底座），只跟随设备旋转。
    加速度积分的位移漂移太大，已去掉（协议字段保留，PC 端不使用）。
  - 键盘/按钮：G 姿态归零，X/Y/Z 让模型绕对应轴转 90°（对齐焊接方向）。
  - 可选：同目录放 model.obj（例如 convert_step.py 转出来的外壳），会用真实
    模型替代简化外壳。面数太多时会自动拒绝，避免软件渲染卡死。

运行：
    python -m pip install PyQt5 numpy
    python tools/motion3d/motion_view.py
"""

import json
import math
import os
import socket
import struct
import sys
import time

import numpy as np
from PyQt5.QtCore import QPointF, QRectF, Qt, QTimer
from PyQt5.QtGui import QColor, QFont, QPainter, QPen, QPolygonF
from PyQt5.QtWidgets import (QApplication, QHBoxLayout, QLabel, QMainWindow,
                             QPushButton, QVBoxLayout, QWidget)

UDP_PORT = 4210
LISTEN_ADDR = "0.0.0.0"

# 设备外壳尺寸（米）：Waveshare ESP32-C5-Touch-LCD-2.8 磁吸底座外壳的近似值
CASE_W, CASE_D, CASE_H = 0.085, 0.062, 0.018
BASE_H = 0.006
SCREEN_W, SCREEN_H = 0.062, 0.046

BG = QColor("#11161b")
GRID = QColor("#1d2831")
CASE_FILL = QColor("#2b3843")
CASE_EDGE = QColor("#4b5d6b")
SCREEN_FILL = QColor("#0d2a3a")
SCREEN_GLOW = QColor("#1585c0")
BASE_FILL = QColor("#202a32")
TEXT = QColor("#c9d6df")
DIM = QColor("#7c8b96")
ACCENT = QColor("#1585c0")

MAX_OBJ_FACES = 20000


# ===================== 数学 =====================


def quat_to_matrix(q):
    w, x, y, z = q
    n = math.sqrt(w * w + x * x + y * y + z * z) or 1.0
    w, x, y, z = w / n, x / n, y / n, z / n
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ], dtype=np.float64)


def quat_mul(a, b):
    aw, ax, ay, az = a
    bw, bx, by, bz = b
    return (aw * bw - ax * bx - ay * by - az * bz,
            aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw)


def quat_axis_angle(axis, deg):
    r = math.radians(deg) * 0.5
    s = math.sin(r)
    return (math.cos(r), axis[0] * s, axis[1] * s, axis[2] * s)


def quat_normalize(q):
    n = math.sqrt(sum(v * v for v in q)) or 1.0
    return tuple(v / n for v in q)


# ===================== 简化模型 =====================

def _box(cx, cy, cz, w, d, h):
    x0, x1 = cx - w / 2, cx + w / 2
    y0, y1 = cy - d / 2, cy + d / 2
    z0, z1 = cz - h / 2, cz + h / 2
    verts = [
        (x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
        (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1),
    ]
    faces = [
        (0, 1, 2, 3),   # 底
        (4, 5, 6, 7),   # 顶
        (0, 1, 5, 4),   # 前
        (1, 2, 6, 5),   # 右
        (2, 3, 7, 6),   # 后
        (3, 0, 4, 7),   # 左
    ]
    return verts, faces


def build_simple_model():
    """返回 (verts, faces, colors)。坐标：X 右，Y 前，Z 上，单位米。"""
    verts = []
    faces = []
    colors = []

    # 磁吸底座（略小一圈，在最下面）
    v, f = _box(0, 0, -CASE_H / 2 - BASE_H / 2, CASE_W * 0.92, CASE_D * 0.92, BASE_H)
    off = len(verts)
    verts += v
    faces += [tuple(i + off for i in face) for face in f]
    colors += [BASE_FILL] * len(f)

    # 外壳
    v, f = _box(0, 0, 0, CASE_W, CASE_D, CASE_H)
    off = len(verts)
    verts += v
    faces += [tuple(i + off for i in face) for face in f]
    colors += [CASE_FILL] * len(f)

    # 屏幕：贴在外壳正面（+Y 面），单独一层，颜色亮一点
    sx, sz = SCREEN_W, SCREEN_H
    y = CASE_D / 2 + 0.0005
    v = [
        (-sx / 2, y, -sz / 2), (sx / 2, y, -sz / 2),
        (sx / 2, y, sz / 2), (-sx / 2, y, sz / 2),
    ]
    off = len(verts)
    verts += v
    faces.append((off + 0, off + 1, off + 2, off + 3))
    colors.append(SCREEN_FILL)

    return np.array(verts, dtype=np.float64), faces, colors


def load_obj(path):
    """极简 OBJ 读取：只取 v 与 f，面数超限就放弃。"""
    verts = []
    faces = []
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            if line.startswith("v "):
                p = line.split()
                if len(p) >= 4:
                    verts.append((float(p[1]), float(p[2]), float(p[3])))
            elif line.startswith("f "):
                idx = []
                for token in line.split()[1:]:
                    i = int(token.split("/")[0])
                    idx.append(i - 1 if i > 0 else len(verts) + i)
                if len(idx) >= 3:
                    faces.append(tuple(idx))
                    if len(faces) > MAX_OBJ_FACES:
                        raise ValueError("OBJ 面数超过 %d，软件渲染会卡" % MAX_OBJ_FACES)

    if not verts or not faces:
        raise ValueError("OBJ 里没有可用的 v/f")

    v = np.array(verts, dtype=np.float64)
    # 归一化到设备大小：以最长边对齐 0.085m，并让模型中心在原点
    center = (v.max(axis=0) + v.min(axis=0)) / 2
    v -= center
    extent = float((v.max(axis=0) - v.min(axis=0)).max()) or 1.0
    v *= (CASE_W / extent)

    # 把 OBJ 常见的 Y 向上转成程序的 Z 向上
    rot = np.array([[1, 0, 0], [0, 0, -1], [0, 1, 0]], dtype=np.float64)
    v = v @ rot.T

    colors = [QColor("#2b3843")] * len(faces)
    return v, faces, colors


# ===================== UDP 接收 =====================


class MotionReceiver:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((LISTEN_ADDR, UDP_PORT))
        self.sock.setblocking(False)
        self.last_rx = 0.0
        self.sample = None
        self.packets = 0
        self.lost = 0
        self._last_seq = None

    def poll(self):
        """把缓冲区里积压的包一次读完，只保留最新一帧。"""
        got = None
        while True:
            try:
                data, _ = self.sock.recvfrom(512)
            except BlockingIOError:
                break
            except OSError:
                break
            try:
                text = data.decode("ascii", "ignore").strip()
                p = text.split()
                # "MOTION" + 12 个整数
                if len(p) != 13 or p[0] != "MOTION":
                    continue
                seq = int(p[1])
                if self._last_seq is not None and seq > self._last_seq + 1:
                    self.lost += seq - self._last_seq - 1
                self._last_seq = seq
                got = {
                    "seq": seq,
                    "t_ms": int(p[2]),
                    "q": (int(p[3]) / 10000.0, int(p[4]) / 10000.0,
                          int(p[5]) / 10000.0, int(p[6]) / 10000.0),
                    "a": (int(p[7]) / 1000.0, int(p[8]) / 1000.0,
                          int(p[9]) / 1000.0),
                    "g": (int(p[10]) / 100.0, int(p[11]) / 100.0, 0.0),
                }
            except (ValueError, IndexError):
                continue

        if got is not None:
            self.sample = got
            self.last_rx = time.time()
            self.packets += 1
        return self.sample

    @property
    def online(self):
        return (time.time() - self.last_rx) < 0.6


# ===================== 3D 视图 =====================


class SceneView(QWidget):
    def __init__(self, model):
        super().__init__()
        self.verts, self.faces, self.colors = model
        self.q = (1.0, 0.0, 0.0, 0.0)   # 设备姿态
        self.q_offset = (1.0, 0.0, 0.0, 0.0)  # 手工对齐偏移
        self.show_grid = True

    def zero_attitude(self):
        """把当前姿态记成「正」：叠加它的共轭，使显示姿态回到之前的轴对齐偏移。

        先按 X/Y/Z 调好模型朝向，再按 G 归零，这样两者不会互相覆盖。
        """
        w, x, y, z = self.q
        self.q_offset = quat_normalize(quat_mul(self.q_offset, (w, -x, -y, -z)))

    def rotate_offset(self, axis, deg):
        self.q_offset = quat_normalize(
            quat_mul(quat_axis_angle(axis, deg), self.q_offset))

    def update_sample(self, sample):
        if sample is None:
            return
        # 只跟姿态；加速度积分出来的位移漂移太大，已经去掉。
        self.q = sample["q"]

    # -------- 投影 --------
    def _project(self, pts):
        # 相机：绕世界原点，从右前上方看
        cam_dir = np.array([0.55, -0.8, 0.45])
        cam_dir /= np.linalg.norm(cam_dir)
        dist = 0.42

        forward = -cam_dir
        up = np.array([0.0, 0.0, 1.0])
        right = np.cross(up, forward)
        right /= np.linalg.norm(right)
        up2 = np.cross(forward, right)

        rel = pts - cam_dir * dist
        x = rel @ right
        y = rel @ up2
        z = rel @ forward
        z = np.maximum(z, 0.02)

        f = 1.9
        sx = x / z * f
        sy = y / z * f

        w = self.width()
        h = self.height()
        scale = min(w, h) * 0.72
        px = w * 0.5 + sx * scale
        py = h * 0.5 - sy * scale
        return px, py, z

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing, True)
        painter.fillRect(self.rect(), BG)

        if self.show_grid:
            self._draw_grid(painter)

        r = quat_to_matrix(quat_mul(self.q_offset, self.q))
        world = self.verts @ r.T
        px, py, pz = self._project(world)

        # 画家算法：远的面先画
        order = []
        for i, face in enumerate(self.faces):
            zc = float(np.mean(pz[list(face)]))
            order.append((zc, i))
        order.sort(reverse=True)

        for zc, i in order:
            face = self.faces[i]
            poly = QPolygonF([QPointF(float(px[j]), float(py[j])) for j in face])

            color = QColor(self.colors[i]) if i < len(self.colors) else CASE_FILL
            # 简单光照：按面法线与相机的夹角调明暗
            p0, p1, p2 = world[face[0]], world[face[1]], world[face[2]]
            n = np.cross(p1 - p0, p2 - p0)
            ln = np.linalg.norm(n)
            if ln > 1e-9:
                n /= ln
                lamp = np.array([0.3, -0.5, 0.9])
                lamp /= np.linalg.norm(lamp)
                shade = 0.45 + 0.55 * abs(float(n @ lamp))
                color = QColor(min(255, int(color.red() * shade)),
                               min(255, int(color.green() * shade)),
                               min(255, int(color.blue() * shade)))

            painter.setBrush(color)
            painter.setPen(QPen(CASE_EDGE, 0.8))
            painter.drawPolygon(poly)

        painter.end()

    def _draw_grid(self, painter):
        painter.setPen(QPen(GRID, 1))
        z = -0.3
        step = 0.08
        n = 5
        for i in range(-n, n + 1):
            p1 = np.array([i * step, -n * step, z])
            p2 = np.array([i * step, n * step, z])
            p3 = np.array([-n * step, i * step, z])
            p4 = np.array([n * step, i * step, z])
            px, py, _ = self._project(np.array([p1, p2, p3, p4]))
            painter.drawLine(QPointF(float(px[0]), float(py[0])),
                             QPointF(float(px[1]), float(py[1])))
            painter.drawLine(QPointF(float(px[2]), float(py[2])),
                             QPointF(float(px[3]), float(py[3])))


# ===================== 主窗口 =====================


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("ESP32-C5 姿态 3D 显示 · UDP %d" % UDP_PORT)
        self.resize(960, 640)

        try:
            self.model = build_simple_model()
            self.model_name = "简化外壳"
            obj = os.path.join(os.path.dirname(os.path.abspath(__file__)), "model.obj")
            if os.path.exists(obj):
                self.model = load_obj(obj)
                self.model_name = "model.obj"
        except Exception as e:      # 模型坏了也别打不开窗口
            self.model = build_simple_model()
            self.model_name = "简化外壳（OBJ 加载失败：%s）" % e

        self.view = SceneView(self.model)
        self.receiver = MotionReceiver()
        if self.model_name.startswith("简化"):
            print("3D 视图：简化外壳（把 model.obj 放在同目录可替换）")
        else:
            print("3D 视图：%s" % self.model_name)

        central = QWidget()
        root = QHBoxLayout(central)
        root.setContentsMargins(12, 12, 12, 12)
        root.setSpacing(12)

        root.addWidget(self.view, 1)

        side = QVBoxLayout()
        side.setSpacing(8)
        root.addLayout(side, 0)

        self.status = QLabel("等待设备数据…")
        self.status.setWordWrap(True)
        self.status.setFont(QFont("Consolas", 10))
        self.status.setStyleSheet("color:#c9d6df;")
        side.addWidget(self.status)

        self.detail = QLabel("")
        self.detail.setWordWrap(True)
        self.detail.setFont(QFont("Consolas", 9))
        self.detail.setStyleSheet("color:#7c8b96;")
        side.addWidget(self.detail)

        side.addStretch(1)

        def add_btn(text, fn):
            b = QPushButton(text)
            b.clicked.connect(fn)
            side.addWidget(b)

        add_btn("姿态归零 (G)", self.view.zero_attitude)
        add_btn("模型绕 X +90°", lambda: self.view.rotate_offset((1, 0, 0), 90))
        add_btn("模型绕 Y +90°", lambda: self.view.rotate_offset((0, 1, 0), 90))
        add_btn("模型绕 Z +90°", lambda: self.view.rotate_offset((0, 0, 1), 90))

        self.setCentralWidget(central)

        self.timer = QTimer(self)
        self.timer.timeout.connect(self.tick)
        self.timer.start(33)

    def tick(self):
        sample = self.receiver.poll()
        if sample is not None:
            self.view.update_sample(sample)

        if self.receiver.online and sample is not None:
            q = sample["q"]
            a = sample["a"]
            g = sample["g"]
            self.status.setText(
                "● 在线   累计 %d 包，丢 %d\n"
                "四元数  w=%.3f x=%.3f y=%.3f z=%.3f\n"
                "加速度  %.2f  %.2f  %.2f g\n"
                "角速度  %.1f  %.1f °/s"
                % (self.receiver.packets, self.receiver.lost,
                   q[0], q[1], q[2], q[3],
                   a[0], a[1], a[2],
                   g[0], g[1]))
        elif not self.receiver.online:
            self.status.setText("○ 等待设备数据…\n"
                                "设备上打开：设置 → 关于 → 姿态广播\n"
                                "确保电脑和设备在同一个 WiFi 下，防火墙允许 UDP %d"
                                % UDP_PORT)

        self.view.update()

    def keyPressEvent(self, e):
        k = e.key()
        if k == Qt.Key_G:
            self.view.zero_attitude()
        elif k == Qt.Key_X:
            self.view.rotate_offset((1, 0, 0), 90)
        elif k == Qt.Key_Y:
            self.view.rotate_offset((0, 1, 0), 90)
        elif k == Qt.Key_Z:
            self.view.rotate_offset((0, 0, 1), 90)
        else:
            super().keyPressEvent(e)


def main():
    app = QApplication(sys.argv)
    w = MainWindow()
    w.show()
    return app.exec_()


if __name__ == "__main__":
    sys.exit(main())
