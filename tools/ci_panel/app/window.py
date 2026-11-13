# -*- coding: utf-8 -*-
"""CI 固件助手主窗口。"""

from __future__ import annotations

import ctypes
import html
import re
import subprocess
import sys
from pathlib import Path

from PySide6.QtCore import QTime, QThread, QTimer, Qt, QUrl
from PySide6.QtGui import QColor, QDesktopServices, QFont, QGuiApplication, QTextCursor
from PySide6.QtSerialPort import QSerialPortInfo
from PySide6.QtWidgets import (
    QAbstractItemView, QApplication, QCheckBox, QComboBox, QDialog,
    QFrame, QHBoxLayout, QHeaderView, QLabel, QLineEdit, QMainWindow,
    QMessageBox, QPlainTextEdit, QProgressBar, QPushButton, QSystemTrayIcon,
    QTableWidget, QTableWidgetItem, QTextEdit, QVBoxLayout, QWidget,
)

from . import theme
from .esptool_util import EsptoolTool, find_esptool
from .github_api import WorkflowRun, average_duration_seconds
from .settings import Prefs
from .workers import (
    ArtifactWorker, BaseWorker, FlashWorker, GitPushWorker, LogsWorker,
    OtaPushWorker, OtaScanWorker, RunsWorker,
)

REPO_ROOT = Path(__file__).resolve().parents[3]
DOWNLOAD_ROOT = REPO_ROOT / "build" / "ci"
DEFAULT_REPO = "moruacat/ESP32-C5-LCD2.8-Touch-Reader"
_REPO_RE = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")

_STATUS_COLORS = {
    "ok": theme.OK,
    "err": theme.ERR,
    "warn": theme.WARN,
    "info": theme.INFO,
    "muted": theme.MUTED,
}
_LOG_COLORS = {
    "info": "#C9D2E1",
    "muted": theme.MUTED,
    "accent": theme.INFO,
    "ok": theme.OK,
    "warn": theme.WARN,
    "error": theme.ERR,
}


def _caption(text: str = "", wrap: bool = False) -> QLabel:
    label = QLabel(text)
    label.setObjectName("caption")
    label.setWordWrap(wrap)
    return label


def _divider() -> QFrame:
    line = QFrame()
    line.setObjectName("divider")
    line.setFixedHeight(1)
    return line


class LogsDialog(QDialog):
    """只读日志查看器。"""

    def __init__(self, parent: QWidget, title: str, text: str) -> None:
        super().__init__(parent)
        self._text = text
        self.setWindowTitle("运行日志")
        self.resize(1000, 720)
        self.setModal(True)

        box = QVBoxLayout(self)
        box.setContentsMargins(18, 16, 18, 14)
        box.setSpacing(10)

        heading = QLabel(title)
        heading.setObjectName("cardTitle")
        heading.setWordWrap(True)
        box.addWidget(heading)
        box.addWidget(_caption("GitHub Actions 日志包已解压；过长时只显示末尾部分。"))

        self.editor = QPlainTextEdit()
        self.editor.setObjectName("logView")
        self.editor.setReadOnly(True)
        self.editor.setLineWrapMode(QPlainTextEdit.NoWrap)
        self.editor.setPlainText(text)
        self.editor.moveCursor(QTextCursor.Start)
        box.addWidget(self.editor, 1)

        buttons = QHBoxLayout()
        buttons.addStretch(1)
        copy_btn = QPushButton("复制全部")
        copy_btn.clicked.connect(self._copy_all)
        close_btn = QPushButton("关闭")
        close_btn.setProperty("variant", "primary")
        close_btn.clicked.connect(self.accept)
        buttons.addWidget(copy_btn)
        buttons.addWidget(close_btn)
        box.addLayout(buttons)

    def _copy_all(self) -> None:
        QApplication.clipboard().setText(self._text)


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.prefs = Prefs()
        self._runs: list[WorkflowRun] = []
        self._runs_by_id: dict[int, WorkflowRun] = {}
        self._current_run: WorkflowRun | None = None
        self._last_download: dict | None = None
        self._busy = False
        self._thread: QThread | None = None
        self._worker: BaseWorker | None = None
        self._updating_table = False
        self._esptool: EsptoolTool | None = None
        self._token_warning = ""
        self._dark_title_done = False
        # 上一次刷新时每个 run 的状态：用来发现「刚跑完」的那一次
        self._run_status_by_id: dict[int, str] = {}
        # 已完成运行的平均耗时（秒），用来估算进行中构建的完成时间
        self._avg_build_seconds: float | None = None
        self._tray: QSystemTrayIcon | None = None

        self._auto_timer = QTimer(self)
        self._auto_timer.setInterval(30_000)
        self._auto_timer.timeout.connect(self._on_auto_tick)

        self.setWindowTitle("CI 固件助手 — GitHub Actions")
        self.setWindowIcon(theme.app_icon(128))
        self.setMinimumSize(1060, 760)
        self.resize(1200, 840)

        self._build_ui()
        self._load_preferences()
        self._setup_tray()
        self._refresh_ports()
        self._refresh_esptool_label()
        self._update_actions()
        self._append_log("就绪。填入 GitHub Token 后可下载 CI artifact。", "muted")
        QTimer.singleShot(450, lambda: self.refresh_runs(manual=False))

    # ================================================================
    # 界面搭建
    # ================================================================

    def _build_ui(self) -> None:
        root = QWidget()
        root.setObjectName("root")
        self.setCentralWidget(root)

        outer = QVBoxLayout(root)
        outer.setContentsMargins(20, 18, 20, 16)
        outer.setSpacing(12)

        outer.addLayout(self._build_header())
        outer.addWidget(self._build_connection_card())

        middle = QHBoxLayout()
        middle.setSpacing(12)
        middle.addWidget(self._build_runs_card(), 1)
        middle.addWidget(self._build_action_card(), 0)
        outer.addLayout(middle, 1)

        bottom = QHBoxLayout()
        bottom.setSpacing(12)
        bottom.addWidget(self._build_log_card(), 1)
        bottom.addWidget(self._build_flash_card(), 0)
        outer.addLayout(bottom, 0)

    def _build_header(self) -> QHBoxLayout:
        row = QHBoxLayout()
        row.setSpacing(12)

        texts = QVBoxLayout()
        texts.setSpacing(2)
        title = QLabel("CI 固件助手")
        title.setObjectName("h1")
        subtitle = QLabel("GitHub Actions 构建查看 · firmware 下载 · 串口烧录")
        subtitle.setObjectName("subtitle")
        texts.addWidget(title)
        texts.addWidget(subtitle)
        row.addLayout(texts, 1)

        badge = QLabel("GitHub Actions")
        badge.setObjectName("badge")
        row.addWidget(badge, 0, Qt.AlignVCenter)
        return row

    def _build_connection_card(self) -> QFrame:
        card = QFrame()
        card.setObjectName("card")
        box = QVBoxLayout(card)
        box.setContentsMargins(15, 13, 15, 13)
        box.setSpacing(9)

        token_row = QHBoxLayout()
        token_row.setSpacing(8)
        token_label = QLabel("GitHub Token")
        token_label.setObjectName("cardTitle")
        token_label.setFixedWidth(104)
        self.token_edit = QLineEdit()
        self.token_edit.setEchoMode(QLineEdit.Password)
        self.token_edit.setPlaceholderText(
            "ghp_... 或 github_pat_...（下载 artifact 必需；公开仓库查询可留空）")
        self.show_token_btn = QPushButton("显示")
        self.show_token_btn.setProperty("variant", "ghost")
        self.show_token_btn.setFixedWidth(56)
        self.show_token_btn.clicked.connect(self._toggle_token_visibility)
        self.remember_cb = QCheckBox("记住 Token")
        self.remember_cb.setToolTip("使用 Windows DPAPI 按当前用户加密后保存在本机，不会写入仓库。")
        self.auto_refresh_cb = QCheckBox("自动刷新")
        self.auto_refresh_cb.setToolTip("每 30 秒刷新一次构建列表；建议填写 Token 后开启。")
        self.auto_refresh_cb.toggled.connect(self._on_auto_refresh_toggled)
        token_row.addWidget(token_label)
        token_row.addWidget(self.token_edit, 1)
        token_row.addWidget(self.show_token_btn)
        token_row.addWidget(self.remember_cb)
        token_row.addWidget(self.auto_refresh_cb)
        box.addLayout(token_row)

        repo_row = QHBoxLayout()
        repo_row.setSpacing(8)
        repo_label = QLabel("仓库")
        repo_label.setObjectName("cardTitle")
        repo_label.setFixedWidth(104)
        self.repo_edit = QLineEdit()
        self.repo_edit.setPlaceholderText("owner/repo")
        self.branch_edit = QLineEdit()
        self.branch_edit.setPlaceholderText("main")
        self.branch_edit.setFixedWidth(120)
        self.refresh_btn = QPushButton("刷新构建")
        self.refresh_btn.setProperty("variant", "primary")
        self.refresh_btn.setFixedWidth(108)
        self.refresh_btn.clicked.connect(self._on_refresh_clicked)
        self.actions_btn = QPushButton("打开 Actions")
        self.actions_btn.setProperty("variant", "ghost")
        self.actions_btn.clicked.connect(self._open_actions_page)
        self.push_btn = QPushButton("推送")
        self.push_btn.setProperty("variant", "ghost")
        self.push_btn.setFixedWidth(64)
        self.push_btn.setToolTip("推送当前本地分支到 origin；只推送已提交内容，不会自动提交。")
        self.push_btn.clicked.connect(self._push_git)
        repo_row.addWidget(repo_label)
        repo_row.addWidget(self.repo_edit, 1)
        branch_label = QLabel("分支")
        branch_label.setObjectName("muted")
        repo_row.addWidget(branch_label)
        repo_row.addWidget(self.branch_edit)
        repo_row.addWidget(self.refresh_btn)
        repo_row.addWidget(self.push_btn)
        repo_row.addWidget(self.actions_btn)
        box.addLayout(repo_row)

        hint = _caption(
            "Token 使用 Windows DPAPI 加密保存在本机，不会写进仓库；"
            "artifact 下载由本工具直接请求 GitHub。", True)
        box.addWidget(hint)
        return card

    def _build_runs_card(self) -> QFrame:
        card = QFrame()
        card.setObjectName("card")
        box = QVBoxLayout(card)
        box.setContentsMargins(14, 13, 14, 12)
        box.setSpacing(9)

        header = QHBoxLayout()
        title = QLabel("最近构建")
        title.setObjectName("cardTitle")
        self.runs_badge = QLabel("—")
        self.runs_badge.setObjectName("badge")
        self.refresh_time_label = _caption("")
        header.addWidget(title)
        header.addWidget(self.runs_badge)
        header.addStretch(1)
        header.addWidget(self.refresh_time_label)
        box.addLayout(header)

        self.runs_table = QTableWidget(0, 5)
        self.runs_table.setHorizontalHeaderLabels(["状态", "提交", "时间", "事件", "标题"])
        self.runs_table.setAlternatingRowColors(True)
        self.runs_table.setShowGrid(False)
        self.runs_table.setWordWrap(False)
        self.runs_table.setEditTriggers(QAbstractItemView.NoEditTriggers)
        self.runs_table.setSelectionBehavior(QAbstractItemView.SelectRows)
        self.runs_table.setSelectionMode(QAbstractItemView.SingleSelection)
        self.runs_table.setVerticalScrollMode(QAbstractItemView.ScrollPerPixel)
        self.runs_table.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.runs_table.verticalHeader().setVisible(False)
        self.runs_table.verticalHeader().setDefaultSectionSize(35)
        self.runs_table.horizontalHeader().setHighlightSections(False)
        self.runs_table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeToContents)
        self.runs_table.horizontalHeader().setSectionResizeMode(4, QHeaderView.Stretch)
        self.runs_table.itemSelectionChanged.connect(self._on_selection_changed)
        self.runs_table.doubleClicked.connect(lambda _index: self._open_selected_run())
        box.addWidget(self.runs_table, 1)

        box.addWidget(_caption("双击构建行可打开对应的 GitHub Actions 页面。"))
        return card

    def _build_action_card(self) -> QFrame:
        card = QFrame()
        card.setObjectName("card")
        card.setFixedWidth(352)
        box = QVBoxLayout(card)
        box.setContentsMargins(15, 12, 15, 13)
        box.setSpacing(7)

        title = QLabel("当前构建")
        title.setObjectName("cardTitle")
        box.addWidget(title)

        self.detail_title = QLabel("尚未选择构建")
        self.detail_title.setWordWrap(True)
        self.detail_title.setStyleSheet(f"font-size: 14px; font-weight: 600; color: {theme.TEXT};")
        box.addWidget(self.detail_title)

        self.detail_status = QLabel("—")
        self.detail_status.setStyleSheet(f"color: {theme.MUTED}; font-weight: 600;")
        box.addWidget(self.detail_status)

        self.detail_meta = _caption("从左侧选择一次 GitHub Actions 运行。", True)
        box.addWidget(self.detail_meta)

        self.detail_sha = QLabel("—")
        self.detail_sha.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.detail_sha.setFont(QFont(theme.MONO_FONT, 9))
        self.detail_sha.setStyleSheet(f"color: {theme.MUTED};")
        box.addWidget(self.detail_sha)

        box.addWidget(_divider())

        self.open_run_btn = QPushButton("打开运行页面")
        self.open_run_btn.clicked.connect(self._open_selected_run)
        self.logs_btn = QPushButton("查看完整日志")
        self.logs_btn.clicked.connect(self._open_logs)
        action_row = QHBoxLayout()
        action_row.setSpacing(7)
        action_row.addWidget(self.open_run_btn, 1)
        action_row.addWidget(self.logs_btn, 1)
        box.addLayout(action_row)

        self.download_btn = QPushButton("下载 firmware")
        self.download_btn.setProperty("variant", "primary")
        self.download_btn.clicked.connect(self._download_firmware)
        box.addWidget(self.download_btn)

        self.download_status = _caption("尚未下载 firmware。", True)
        box.addWidget(self.download_status)
        return card

    def _build_flash_card(self) -> QFrame:
        card = QFrame()
        card.setObjectName("card")
        card.setFixedWidth(352)
        box = QVBoxLayout(card)
        box.setContentsMargins(15, 12, 15, 13)
        box.setSpacing(8)

        title = QLabel("固件烧录")
        title.setObjectName("cardTitle")
        box.addWidget(title)

        self.esptool_label = _caption("正在检测 esptool ...", False)
        box.addWidget(self.esptool_label)

        port_row = QHBoxLayout()
        port_row.setSpacing(7)
        port_label = QLabel("串口")
        port_label.setObjectName("muted")
        self.port_combo = QComboBox()
        self.port_combo.setEditable(True)
        self.port_combo.setMinimumWidth(120)
        self.refresh_ports_btn = QPushButton("刷新")
        self.refresh_ports_btn.setProperty("variant", "ghost")
        self.refresh_ports_btn.setFixedWidth(54)
        self.refresh_ports_btn.clicked.connect(lambda: self._refresh_ports())
        port_row.addWidget(port_label)
        port_row.addWidget(self.port_combo, 1)
        port_row.addWidget(self.refresh_ports_btn)
        box.addLayout(port_row)

        self.flash_btn = QPushButton("烧录到设备")
        self.flash_btn.setProperty("variant", "danger")
        self.flash_btn.setToolTip("烧录会覆盖设备当前固件；点击后还会再次确认。")
        self.flash_btn.clicked.connect(self._flash_firmware)
        self.open_folder_btn = QPushButton("打开目录")
        self.open_folder_btn.setProperty("variant", "ghost")
        self.open_folder_btn.setToolTip("打开最近一次下载的 firmware 目录。")
        self.open_folder_btn.clicked.connect(self._open_download_folder)
        flash_row = QHBoxLayout()
        flash_row.setSpacing(7)
        flash_row.addWidget(self.flash_btn, 2)
        flash_row.addWidget(self.open_folder_btn, 1)
        box.addLayout(flash_row)

        # ===== 局域网 OTA（方案见 docs/ota_eval.md）=====
        box.addWidget(_divider())

        ota_title = QLabel("局域网 OTA")
        ota_title.setObjectName("cardTitle")
        box.addWidget(ota_title)

        self.ota_hint = _caption("点「刷新设备」扫描同一 WiFi 下的 ClockReader。", True)
        box.addWidget(self.ota_hint)

        ota_row = QHBoxLayout()
        ota_row.setSpacing(7)
        self.ota_combo = QComboBox()
        self.ota_combo.setMinimumWidth(150)
        self.ota_scan_btn = QPushButton("刷新设备")
        self.ota_scan_btn.setProperty("variant", "ghost")
        self.ota_scan_btn.setFixedWidth(72)
        self.ota_scan_btn.setToolTip("用 mDNS 扫描 _clockreader-ota._tcp，要求设备与电脑在同一网段。")
        self.ota_scan_btn.clicked.connect(self._on_ota_scan)
        ota_row.addWidget(self.ota_combo, 1)
        ota_row.addWidget(self.ota_scan_btn)
        box.addLayout(ota_row)

        self.ota_btn = QPushButton("OTA 推送固件")
        self.ota_btn.setProperty("variant", "primary")
        self.ota_btn.setToolTip(
            "把已下载的固件推给选中的设备。\n"
            "设备端 OTA 服务尚未实现时会在探测阶段报错（见 docs/ota_eval.md）。")
        self.ota_btn.clicked.connect(self._on_ota_push)
        box.addWidget(self.ota_btn)
        return card

    def _build_log_card(self) -> QFrame:
        card = QFrame()
        card.setObjectName("card")
        box = QVBoxLayout(card)
        box.setContentsMargins(14, 11, 14, 11)
        box.setSpacing(7)

        header = QHBoxLayout()
        title = QLabel("任务日志")
        title.setObjectName("cardTitle")
        self.clear_log_btn = QPushButton("清空")
        self.clear_log_btn.setProperty("variant", "ghost")
        self.clear_log_btn.clicked.connect(lambda: self.log_edit.clear())
        header.addWidget(title)
        header.addStretch(1)
        header.addWidget(self.clear_log_btn)
        box.addLayout(header)

        self.log_edit = QTextEdit()
        self.log_edit.setObjectName("log")
        self.log_edit.setReadOnly(True)
        self.log_edit.setAcceptRichText(True)
        self.log_edit.setFixedHeight(108)
        self.log_edit.setPlaceholderText("操作日志会显示在这里。")
        box.addWidget(self.log_edit)

        footer = QHBoxLayout()
        footer.setSpacing(10)
        self.progress_bar = QProgressBar()
        self.progress_bar.setTextVisible(False)
        self.progress_bar.setVisible(False)
        self.status_label = _caption("就绪。", True)
        footer.addWidget(self.progress_bar, 1)
        footer.addWidget(self.status_label, 2)
        box.addLayout(footer)
        return card

    # ================================================================
    # 设置与状态
    # ================================================================

    def _load_preferences(self) -> None:
        self.repo_edit.setText(self.prefs.text("github/repo", DEFAULT_REPO))
        self.branch_edit.setText(self.prefs.text("github/branch", "main"))
        self.remember_cb.setChecked(self.prefs.boolean("github/remember_token", False))

        token, warning = self.prefs.load_token()
        self.token_edit.setText(token)
        self._token_warning = warning
        if warning:
            self._append_log(warning, "warn")

        auto_refresh = self.prefs.boolean("ui/auto_refresh", False)
        self.auto_refresh_cb.setChecked(auto_refresh)
        if auto_refresh:
            self._auto_timer.start()

        geometry = self.prefs.geometry()
        if geometry is None or not self.restoreGeometry(geometry):
            screen = QGuiApplication.primaryScreen()
            if screen is not None:
                area = screen.availableGeometry()
                width = min(1200, int(area.width() * 0.92))
                height = min(820, int(area.height() * 0.92))
                self.resize(width, height)
                self.move(area.x() + (area.width() - width) // 2,
                          area.y() + (area.height() - height) // 2)

    def _save_preferences(self) -> None:
        self.prefs.set("github/repo", self.repo_edit.text().strip())
        self.prefs.set("github/branch", self.branch_edit.text().strip() or "main")
        self.prefs.set("ui/auto_refresh", self.auto_refresh_cb.isChecked())
        self.prefs.set("flash/port", self.port_combo.currentText().strip())
        self.prefs.set_geometry(self.saveGeometry())
        warning = self.prefs.save_token(
            self.token_edit.text().strip(), self.remember_cb.isChecked())
        if warning:
            self._token_warning = warning
        self.prefs.sync()

    def _toggle_token_visibility(self) -> None:
        if self.token_edit.echoMode() == QLineEdit.Password:
            self.token_edit.setEchoMode(QLineEdit.Normal)
            self.show_token_btn.setText("隐藏")
        else:
            self.token_edit.setEchoMode(QLineEdit.Password)
            self.show_token_btn.setText("显示")

    def _on_auto_refresh_toggled(self, checked: bool) -> None:
        self.prefs.set("ui/auto_refresh", bool(checked))
        if checked:
            self._auto_timer.start()
        else:
            self._auto_timer.stop()

    def _on_auto_tick(self) -> None:
        if self._busy or not self.auto_refresh_cb.isChecked():
            return
        if not self.token_edit.text().strip():
            return
        self.refresh_runs(manual=False)

    def _set_status(self, text: str, color: str = theme.MUTED) -> None:
        self.status_label.setText(text)
        self.status_label.setStyleSheet(f"color: {color};")

    def _append_log(self, text: str, level: str = "info") -> None:
        if not text:
            return
        color = _LOG_COLORS.get(level, _LOG_COLORS["info"])
        stamp = QTime.currentTime().toString("HH:mm:ss")
        body = html.escape(str(text)).replace("\n", "<br>")
        self.log_edit.append(
            f'<span style="color:{theme.FAINT}">[{stamp}]</span> '
            f'<span style="color:{color}">{body}</span>')
        bar = self.log_edit.verticalScrollBar()
        bar.setValue(bar.maximum())

    def _set_controls_enabled(self, enabled: bool) -> None:
        widgets = [
            self.token_edit, self.repo_edit, self.branch_edit,
            self.show_token_btn, self.remember_cb, self.auto_refresh_cb,
            self.refresh_btn, self.actions_btn, self.push_btn, self.runs_table,
            self.open_run_btn, self.logs_btn, self.download_btn,
            self.port_combo, self.refresh_ports_btn, self.flash_btn,
            self.open_folder_btn, self.ota_combo, self.ota_scan_btn, self.ota_btn,
        ]
        for widget in widgets:
            widget.setEnabled(enabled)

    def _update_actions(self) -> None:
        if self._busy:
            return
        run = self._current_run
        self.refresh_btn.setEnabled(True)
        self.actions_btn.setEnabled(True)
        self.push_btn.setEnabled(True)
        self.token_edit.setEnabled(True)
        self.repo_edit.setEnabled(True)
        self.branch_edit.setEnabled(True)
        self.show_token_btn.setEnabled(True)
        self.remember_cb.setEnabled(True)
        self.auto_refresh_cb.setEnabled(True)
        self.refresh_ports_btn.setEnabled(True)
        self.port_combo.setEnabled(True)
        self.open_run_btn.setEnabled(run is not None)
        self.logs_btn.setEnabled(run is not None)
        self.download_btn.setEnabled(run is not None and run.conclusion == "success")
        self.flash_btn.setEnabled(self._last_download is not None)
        self.open_folder_btn.setEnabled(self._last_download is not None)
        self.ota_scan_btn.setEnabled(True)
        self.ota_btn.setEnabled(self._last_download is not None
                                and self.ota_combo.count() > 0)

    # ================================================================
    # 后台任务
    # ================================================================

    def _start_task(self, worker: BaseWorker, on_done, busy_text: str) -> bool:
        if self._busy:
            self._append_log("已有任务在运行，请等待完成。", "warn")
            return False
        self._busy = True
        self._set_controls_enabled(False)
        self.progress_bar.setVisible(True)
        self.progress_bar.setRange(0, 0)
        self._set_status(busy_text, theme.INFO)

        thread = QThread(self)
        worker.moveToThread(thread)
        thread.started.connect(worker.run)
        worker.log.connect(self._append_log)
        worker.progress.connect(self._on_worker_progress)
        worker.done.connect(on_done)
        worker.done.connect(thread.quit)
        worker.done.connect(worker.deleteLater)
        thread.finished.connect(thread.deleteLater)
        thread.finished.connect(self._on_task_finished)
        self._thread = thread
        self._worker = worker
        thread.start()
        return True

    def _on_worker_progress(self, done: int, total: int, message: str) -> None:
        if total > 0:
            self.progress_bar.setRange(0, total)
            self.progress_bar.setValue(max(0, min(done, total)))
        else:
            self.progress_bar.setRange(0, 0)
        if message:
            self._set_status(message, theme.INFO)

    def _on_task_finished(self) -> None:
        self._thread = None
        self._worker = None
        self._busy = False
        self.progress_bar.setVisible(False)
        self._set_controls_enabled(True)
        self._update_actions()

    # ================================================================
    # 构建列表
    # ================================================================

    def _on_refresh_clicked(self, _checked: bool = False) -> None:
        self.refresh_runs(manual=True)

    def refresh_runs(self, manual: bool = True) -> None:
        if self._busy:
            return
        repo = self.repo_edit.text().strip()
        branch = self.branch_edit.text().strip() or "main"
        if not _REPO_RE.fullmatch(repo):
            if manual:
                QMessageBox.warning(self, "仓库格式不正确",
                                    "请按 owner/repo 填写，例如：\n"
                                    "moruacat/ESP32-C5-LCD2.8-Touch-Reader")
            return

        token = self.token_edit.text().strip()
        if manual:
            self._save_connection()
        worker = RunsWorker(repo, branch, token)
        self._start_task(worker, self._on_runs_done, "正在查询 GitHub Actions 构建...")

    def _save_connection(self) -> None:
        self.prefs.set("github/repo", self.repo_edit.text().strip())
        self.prefs.set("github/branch", self.branch_edit.text().strip() or "main")
        warning = self.prefs.save_token(
            self.token_edit.text().strip(), self.remember_cb.isChecked())
        if warning and warning != self._token_warning:
            self._token_warning = warning
            self._append_log(warning, "warn")

    def _setup_tray(self) -> None:
        """常驻托盘图标，用来弹「构建完成」通知。

        托盘不可用（被禁用 / 无桌面环境）时静默跳过，完成提醒退化成
        状态栏 + 日志 + 提示音，功能不丢。
        """
        if not QSystemTrayIcon.isSystemTrayAvailable():
            return
        self._tray = QSystemTrayIcon(theme.app_icon(64), self)
        self._tray.setToolTip("CI 固件助手")
        self._tray.activated.connect(self._on_tray_activated)
        self._tray.show()

    def _on_tray_activated(self, reason) -> None:
        """点托盘图标把窗口拉回前台。"""
        if reason == QSystemTrayIcon.Trigger:
            self.showNormal()
            self.raise_()
            self.activateWindow()

    def _check_finished(self, runs: list[WorkflowRun]) -> list[WorkflowRun]:
        """对比上一轮状态，找出这一次刷新里刚结束的构建。

        首次加载时所有旧状态都是 None，不会误报。
        返回刚结束的运行列表，调用方据此决定状态栏要不要让位给完成提示。
        """
        finished = []
        for run in runs:
            old = self._run_status_by_id.get(run.id)
            if old is not None and old != "completed" and run.status == "completed":
                finished.append(run)
        self._run_status_by_id = {run.id: run.status for run in runs}
        if finished:
            self._notify_finished(finished)
        return finished

    def _notify_finished(self, runs: list[WorkflowRun]) -> None:
        """构建完成提醒：状态栏 + 日志 + 提示音 + 托盘气泡。"""
        parts = [f"#{r.run_number} {r.status_text}" for r in runs[:3]]
        if len(runs) > 3:
            parts.append(f"等 {len(runs)} 个")
        text = "、".join(parts)
        ok = all(r.conclusion == "success" for r in runs)

        self._append_log(f"构建完成：{text}", "ok" if ok else "error")
        self._set_status(f"构建完成：{text}", theme.OK if ok else theme.ERR)
        QApplication.beep()

        if self._tray is not None:
            self._tray.showMessage(
                "CI 固件助手",
                f"构建完成：{text}",
                QSystemTrayIcon.Information if ok else QSystemTrayIcon.Warning,
                8000)

    def _on_runs_done(self, data: dict) -> None:
        error = data.get("error")
        if error:
            self._set_status(f"刷新失败：{error}", theme.ERR)
            return
        runs = data.get("runs") or []
        self._runs = runs
        self._runs_by_id = {run.id: run for run in runs}
        self._avg_build_seconds = average_duration_seconds(runs)
        finished = self._check_finished(runs)
        self.runs_badge.setText(f"{len(runs)} 条")
        self.refresh_time_label.setText("刷新于 " + QTime.currentTime().toString("HH:mm:ss"))
        previous = self._current_run.id if self._current_run else None
        self._populate_runs(runs, previous)
        if not finished:
            # 刚有构建结束时状态栏已经是「构建完成：…」，别用刷新提示盖掉它
            self._set_status(f"构建列表已刷新，共 {len(runs)} 条。", theme.OK)

    def _populate_runs(self, runs: list[WorkflowRun], preserve_id: int | None) -> None:
        self._updating_table = True
        try:
            self.runs_table.setRowCount(0)
            self.runs_table.setRowCount(len(runs))
            for row, run in enumerate(runs):
                status_item = self._item(
                    run.status_text, _STATUS_COLORS.get(run.status_kind, theme.MUTED), bold=True)
                status_item.setData(Qt.UserRole, run.id)
                sha_item = self._item(run.short_sha, theme.MUTED, mono=True)
                time_item = self._item(run.time_text, theme.MUTED)
                event_item = self._item(run.event_text, theme.MUTED)
                title_item = self._item(
                    f"#{run.run_number}  {run.display_title}", theme.TEXT)

                eta = run.eta_text(self._avg_build_seconds)
                tooltip = (
                    f"#{run.run_number}  {run.display_title}\n"
                    f"运行 ID：{run.id}\n"
                    f"工作流：{run.name}\n"
                    f"提交：{run.head_sha or '—'}\n"
                    f"分支：{run.head_branch or '—'}\n"
                    f"状态：{run.status_text}    用时：{run.duration_text or '—'}\n"
                    + (f"{eta}\n" if eta else "")
                    + f"时间：{run.created_at}")
                for item in (status_item, sha_item, time_item, event_item, title_item):
                    item.setToolTip(tooltip)

                self.runs_table.setItem(row, 0, status_item)
                self.runs_table.setItem(row, 1, sha_item)
                self.runs_table.setItem(row, 2, time_item)
                self.runs_table.setItem(row, 3, event_item)
                self.runs_table.setItem(row, 4, title_item)
        finally:
            self._updating_table = False

        target = preserve_id if preserve_id in self._runs_by_id else (runs[0].id if runs else None)
        if target is not None:
            self._select_run(target)
        else:
            self._current_run = None
            self._show_empty_details()
            self._update_actions()

    def _item(self, text: str, color: str, bold: bool = False,
              mono: bool = False) -> QTableWidgetItem:
        item = QTableWidgetItem(text)
        item.setForeground(QColor(color))
        font = item.font()
        font.setBold(bold)
        if mono:
            font.setFamily(theme.MONO_FONT)
        item.setFont(font)
        return item

    def _select_run(self, run_id: int) -> None:
        for row in range(self.runs_table.rowCount()):
            item = self.runs_table.item(row, 0)
            if item is not None and item.data(Qt.UserRole) == run_id:
                self.runs_table.selectRow(row)
                self.runs_table.scrollToItem(item)
                return

    def _on_selection_changed(self) -> None:
        if self._updating_table:
            return
        rows = self.runs_table.selectionModel().selectedRows()
        if not rows:
            self._current_run = None
            self._show_empty_details()
            self._update_actions()
            return
        item = self.runs_table.item(rows[0].row(), 0)
        run_id = item.data(Qt.UserRole) if item is not None else None
        self._current_run = self._runs_by_id.get(run_id)
        self._show_run_details(self._current_run)
        self._update_actions()

    def _show_empty_details(self) -> None:
        self.detail_title.setText("尚未选择构建")
        self.detail_status.setText("—")
        self.detail_status.setStyleSheet(f"color: {theme.MUTED}; font-weight: 600;")
        self.detail_meta.setText("从左侧选择一次 GitHub Actions 运行。")
        self.detail_sha.setText("—")

    def _show_run_details(self, run: WorkflowRun | None) -> None:
        if run is None:
            self._show_empty_details()
            return
        color = _STATUS_COLORS.get(run.status_kind, theme.MUTED)
        self.detail_title.setText(f"#{run.run_number}  {run.display_title}")
        self.detail_status.setText(f"● {run.status_text}")
        self.detail_status.setStyleSheet(f"color: {color}; font-weight: 600;")
        duration = f"\n用时 {run.duration_text}" if run.duration_text else ""
        eta = run.eta_text(self._avg_build_seconds)
        if eta:
            duration += f"\n{eta}"
        self.detail_meta.setText(
            f"{run.name}\n{run.event_text} · {run.time_text}{duration}\n"
            f"分支 {run.head_branch or '—'}")
        self.detail_sha.setText(run.head_sha or "—")
        self.detail_sha.setToolTip(f"完整 SHA：{run.head_sha or '—'}")

    # ================================================================
    # Git 推送
    # ================================================================

    def _git_preflight(self) -> tuple[str, str, str, int] | None:
        """读取当前分支、工作区状态和待推送提交数；失败时弹窗并返回 None。"""
        try:
            branch_result = subprocess.run(
                ["git", "-C", str(REPO_ROOT), "rev-parse", "--abbrev-ref", "HEAD"],
                capture_output=True, text=True, encoding="utf-8",
                errors="replace", timeout=8)
            status_result = subprocess.run(
                ["git", "-C", str(REPO_ROOT), "status", "--porcelain"],
                capture_output=True, text=True, encoding="utf-8",
                errors="replace", timeout=8)
            remote_result = subprocess.run(
                ["git", "-C", str(REPO_ROOT), "remote", "get-url", "origin"],
                capture_output=True, text=True, encoding="utf-8",
                errors="replace", timeout=8)
        except FileNotFoundError:
            QMessageBox.critical(self, "找不到 Git", "当前系统找不到 git 命令，请先安装 Git。")
            return None
        except subprocess.TimeoutExpired:
            QMessageBox.critical(self, "Git 超时", "读取本地 Git 状态超时。")
            return None

        if branch_result.returncode != 0:
            detail = (branch_result.stderr or branch_result.stdout).strip()
            QMessageBox.critical(self, "不是 Git 仓库",
                                 f"无法读取当前分支：\n{detail or '未知错误'}")
            return None
        if remote_result.returncode != 0:
            QMessageBox.critical(self, "缺少 origin",
                                 "当前仓库没有配置 origin 远程。")
            return None

        branch = branch_result.stdout.strip()
        if not branch or branch == "HEAD":
            QMessageBox.warning(self, "当前不是分支",
                                "当前处于 detached HEAD 状态，请先切换到一个分支再推送。")
            return None

        lines = [line for line in status_result.stdout.splitlines() if line.strip()]
        tracked = [line for line in lines if not line.startswith("??")]
        untracked = len(lines) - len(tracked)
        dirty_parts: list[str] = []
        if tracked:
            dirty_parts.append(f"{len(tracked)} 项已跟踪改动")
        if untracked:
            dirty_parts.append(f"{untracked} 项未跟踪文件")
        dirty_text = "、".join(dirty_parts)

        # 先按 upstream 算；没有 upstream 再和 origin/<branch> 比。
        ahead = -1
        for spec in ("@{u}..HEAD", f"origin/{branch}..HEAD"):
            try:
                result = subprocess.run(
                    ["git", "-C", str(REPO_ROOT), "rev-list", "--count", spec],
                    capture_output=True, text=True, encoding="utf-8",
                    errors="replace", timeout=8)
            except (FileNotFoundError, subprocess.TimeoutExpired):
                continue
            if result.returncode == 0:
                try:
                    ahead = int(result.stdout.strip())
                except ValueError:
                    ahead = -1
                break

        remote = remote_result.stdout.strip()
        return branch, dirty_text, remote, ahead

    def _push_git(self) -> None:
        if self._busy:
            return
        preflight = self._git_preflight()
        if preflight is None:
            return
        branch, dirty_text, remote, ahead = preflight

        if ahead == 0:
            if dirty_text:
                QMessageBox.information(
                    self, "没有新提交可推送",
                    f"本地分支与 origin 已同步，当前没有新的 commit。\n\n"
                    f"工作区还有 {dirty_text}，这些内容不会被 git push 推送。\n"
                    "请先在终端执行 git add/commit，或让协作方提交后再点推送。")
            else:
                QMessageBox.information(
                    self, "没有新提交可推送",
                    "本地分支与 origin 已同步，没有新的 commit。")
            self._set_status("没有新提交可推送。", theme.WARN)
            return

        box = QMessageBox(self)
        box.setWindowTitle("推送当前分支")
        box.setIcon(QMessageBox.Question)
        box.setText(f"将执行：git push origin {branch}")
        info = (
            f"本地仓库：{REPO_ROOT}\n"
            f"远程地址：{remote}\n\n"
            "只推送已经提交的内容，不会自动 add/commit。"
        )
        if ahead > 0:
            info += f"\n待推送 commit：{ahead} 个"
        if dirty_text:
            info += f"\n\n工作区还有 {dirty_text}，不会本次推送。"
        info += "\n\n推送需要本机 Git 凭据已配置好。"
        box.setInformativeText(info)
        push = box.addButton("推送", QMessageBox.AcceptRole)
        cancel = box.addButton("取消", QMessageBox.RejectRole)
        box.setDefaultButton(cancel)
        box.exec()
        if box.clickedButton() is not push:
            return

        worker = GitPushWorker(REPO_ROOT, branch)
        self._append_log(f"开始推送本地分支 {branch} 到 origin ...", "accent")
        self._start_task(worker, self._on_push_done, f"正在推送 {branch} ...")

    def _on_push_done(self, data: dict) -> None:
        error = data.get("error")
        if error:
            self._set_status(f"推送失败：{error}", theme.ERR)
            QMessageBox.critical(self, "推送失败", error)
            return

        branch = data.get("branch") or "当前分支"
        self._set_status(f"推送完成：{branch} -> origin", theme.OK)
        QMessageBox.information(
            self, "推送完成",
            f"{branch} 已推送到 origin。\n新提交会触发 GitHub Actions 构建。")

    # ================================================================
    # 打开页面 / 日志
    # ================================================================

    def _open_actions_page(self) -> None:
        repo = self.repo_edit.text().strip()
        if not _REPO_RE.fullmatch(repo):
            QMessageBox.warning(self, "仓库格式不正确", "请先填写正确的 owner/repo。")
            return
        QDesktopServices.openUrl(QUrl(f"https://github.com/{repo}/actions"))

    def _open_selected_run(self) -> None:
        if self._current_run is None or not self._current_run.html_url:
            return
        QDesktopServices.openUrl(QUrl(self._current_run.html_url))

    def _open_logs(self) -> None:
        run = self._current_run
        if run is None:
            return
        token = self.token_edit.text().strip()
        if not token:
            QMessageBox.warning(self, "需要 GitHub Token",
                                "查看完整 Actions 日志需要填写 GitHub Token。")
            self.token_edit.setFocus()
            return
        worker = LogsWorker(
            self.repo_edit.text().strip(), token, run.id,
            f"#{run.run_number}  {run.display_title}")
        self._start_task(worker, self._on_logs_done, "正在下载运行日志...")

    def _on_logs_done(self, data: dict) -> None:
        error = data.get("error")
        if error:
            self._set_status(f"日志获取失败：{error}", theme.ERR)
            QMessageBox.warning(self, "日志获取失败", error)
            return
        text = data.get("text") or ""
        title = data.get("title") or "运行日志"
        self._set_status("日志已下载。", theme.OK)
        dialog = LogsDialog(self, title, text)
        dialog.exec()

    # ================================================================
    # 下载
    # ================================================================

    def _download_firmware(self) -> None:
        run = self._current_run
        if run is None:
            return
        if run.conclusion != "success":
            QMessageBox.information(self, "构建尚未成功",
                                    "只能下载结论为「成功」的构建 artifact。")
            return
        token = self.token_edit.text().strip()
        if not token:
            QMessageBox.warning(self, "需要 GitHub Token",
                                "下载 GitHub Actions artifact 必须填写 Token。\n"
                                "Token 需要 Actions: Read 权限。")
            self.token_edit.setFocus()
            return

        self._save_connection()
        worker = ArtifactWorker(
            self.repo_edit.text().strip(), token, run.id, run.head_sha,
            DOWNLOAD_ROOT)
        self._append_log(f"开始下载运行 #{run.run_number}（{run.short_sha}）的 firmware ...", "accent")
        self._start_task(worker, self._on_download_done, "正在准备下载 firmware...")

    def _on_download_done(self, data: dict) -> None:
        error = data.get("error")
        if error:
            self._set_status(f"下载失败：{error}", theme.ERR)
            QMessageBox.warning(self, "固件下载失败", error)
            return

        self._last_download = data
        run_id = data.get("run_id")
        sha = str(data.get("head_sha") or "")[:7]
        out_dir = Path(str(data.get("out_dir") or ""))
        try:
            shown = str(out_dir.relative_to(REPO_ROOT))
        except ValueError:
            shown = str(out_dir)
        self.download_status.setText(f"运行 #{run_id} · {sha}\n{shown}")
        self.download_status.setStyleSheet(f"color: {theme.OK}; font-size: 11px;")
        self._set_status(f"firmware 下载完成：{shown}", theme.OK)
        self._update_actions()

    def _open_download_folder(self) -> None:
        if not self._last_download:
            return
        out_dir = Path(str(self._last_download.get("out_dir") or ""))
        if not out_dir.exists():
            QMessageBox.warning(self, "目录不存在", "下载目录已不存在，请重新下载 firmware。")
            return
        QDesktopServices.openUrl(QUrl.fromLocalFile(str(out_dir)))

    # ================================================================
    # 串口与烧录
    # ================================================================

    def _refresh_ports(self, preferred: str | None = None) -> None:
        current = (preferred or self.port_combo.currentText().strip() or
                   self.prefs.text("flash/port", "COM20"))
        names: list[str] = []
        for info in QSerialPortInfo.availablePorts():
            name = info.portName().strip()
            if name and name not in names:
                names.append(name)
        self.port_combo.blockSignals(True)
        try:
            self.port_combo.clear()
            self.port_combo.addItems(names)
            if current and current not in names:
                self.port_combo.setEditText(current)
            elif names:
                self.port_combo.setCurrentIndex(
                    names.index(current) if current in names else 0)
            else:
                self.port_combo.setEditText(current or "COM20")
        finally:
            self.port_combo.blockSignals(False)

    def _refresh_esptool_label(self) -> None:
        self._esptool = find_esptool()
        if self._esptool is not None:
            self.esptool_label.setText("烧录引擎：已检测到 esptool")
            self.esptool_label.setStyleSheet(f"color: {theme.OK}; font-size: 11px;")
            self.esptool_label.setToolTip(self._esptool.label)
        else:
            self.esptool_label.setText("烧录引擎：未找到 esptool（下载功能不受影响）")
            self.esptool_label.setStyleSheet(f"color: {theme.WARN}; font-size: 11px;")
            self.esptool_label.setToolTip(
                "可安装 esptool，或在 ESP-IDF 环境中启动。\n"
                "常见路径：C:\\Espressif\\tools\\python\\*\\venv\\Scripts\\esptool.exe")

    def _flash_firmware(self) -> None:
        if not self._last_download:
            return
        port = self.port_combo.currentText().strip()
        if not port:
            QMessageBox.warning(self, "没有选择串口", "请选择或填写设备串口，例如 COM20。")
            return

        tool = find_esptool()
        self._esptool = tool
        if tool is None:
            QMessageBox.warning(
                self, "找不到 esptool",
                "下载功能可用，但当前 Python 环境没有找到 esptool。\n\n"
                "可以安装：python -m pip install esptool\n"
                "或直接在 ESP-IDF 的 Python 环境中启动本工具。")
            self._refresh_esptool_label()
            return
        self.esptool_label.setText("烧录引擎：已检测到 esptool")
        self.esptool_label.setStyleSheet(f"color: {theme.OK}; font-size: 11px;")
        self.esptool_label.setToolTip(tool.label)

        paths = {
            "app": str(self._last_download.get("app") or ""),
            "bootloader": str(self._last_download.get("bootloader") or ""),
            "partition_table": str(self._last_download.get("partition_table") or ""),
            "ota_data": str(self._last_download.get("ota_data") or ""),
        }
        missing = [Path(p) for key, p in paths.items() if key != "ota_data" and not Path(p).is_file()]
        if missing:
            QMessageBox.warning(self, "固件文件缺失",
                                "下载目录中的固件文件不完整，请重新下载。")
            return

        run_id = self._last_download.get("run_id")
        sha = str(self._last_download.get("head_sha") or "")[:7]
        box = QMessageBox(self)
        box.setWindowTitle("确认烧录")
        box.setIcon(QMessageBox.Warning)
        box.setText(f"即将向 {port} 烧录固件")
        box.setInformativeText(
            f"运行 #{run_id} · {sha}\n\n"
            "烧录会覆盖设备当前固件。请确认设备已连接，且烧录过程中不要断开。")
        yes = box.addButton("开始烧录", QMessageBox.AcceptRole)
        cancel = box.addButton("取消", QMessageBox.RejectRole)
        box.setDefaultButton(cancel)
        box.exec()
        if box.clickedButton() is not yes:
            return

        self.prefs.set("flash/port", port)
        worker = FlashWorker(tool, port, paths)
        self._append_log(f"开始烧录到 {port} ...", "accent")
        self._start_task(worker, self._on_flash_done, "正在烧录，请勿断开设备...")

    def _on_flash_done(self, data: dict) -> None:
        error = data.get("error")
        if error:
            self._set_status(f"烧录失败：{error}", theme.ERR)
            QMessageBox.critical(self, "烧录失败", error)
            return
        self._set_status("烧录完成，设备已复位。", theme.OK)
        QMessageBox.information(self, "烧录完成", "固件烧录完成，设备已自动复位。")

    # ================================================================
    # 局域网 OTA（方案见 docs/ota_eval.md）
    # ================================================================

    def _on_ota_scan(self, _checked: bool = False) -> None:
        self.ota_hint.setText("正在扫描局域网 ...")
        self.ota_hint.setStyleSheet(f"color: {theme.MUTED}; font-size: 11px;")
        self._start_task(OtaScanWorker(), self._on_ota_scan_done, "扫描 OTA 设备")

    def _on_ota_scan_done(self, data: dict) -> None:
        error = data.get("error")
        if error:
            self.ota_hint.setText(f"扫描失败：{error}")
            self.ota_hint.setStyleSheet(f"color: {theme.ERR}; font-size: 11px;")
            self._set_status(f"OTA 设备扫描失败：{error}", theme.ERR)
            return

        devices = data.get("devices") or []
        self.ota_combo.clear()
        for device in devices:
            self.ota_combo.addItem(device.label(), device)

        if not devices:
            self.ota_hint.setText("没找到设备：确认设备已开机、连的是同一 WiFi，且固件里已启用 OTA。")
            self.ota_hint.setStyleSheet(f"color: {theme.WARN}; font-size: 11px;")
        else:
            self.ota_hint.setText(f"找到 {len(devices)} 台设备。")
            self.ota_hint.setStyleSheet(f"color: {theme.OK}; font-size: 11px;")
        self._update_actions()

    def _on_ota_push(self, _checked: bool = False) -> None:
        device = self.ota_combo.currentData()
        if device is None:
            QMessageBox.information(self, "没有选择设备",
                                    "先点「刷新设备」，扫描同一 WiFi 下的 ClockReader。")
            return
        if not self._last_download:
            QMessageBox.information(self, "没有固件",
                                    "先下载一次成功构建的 firmware，再来推送。")
            return

        app_bin = Path(str(self._last_download.get("app") or ""))
        if not app_bin.is_file():
            QMessageBox.warning(self, "固件文件不存在",
                                f"找不到 {app_bin}，请重新下载 firmware。")
            return

        size_mb = app_bin.stat().st_size / 1048576
        answer = QMessageBox.question(
            self, "确认 OTA 推送",
            f"把 {app_bin.name}（{size_mb:.2f} MB）推送到：\n{device.label()}\n\n"
            "推送成功后设备会自动重启，期间不要断电。",
            QMessageBox.Yes | QMessageBox.No, QMessageBox.No)
        if answer != QMessageBox.Yes:
            return

        worker = OtaPushWorker(device.host, device.port, app_bin,
                               self.prefs.text("ota/token", ""))
        self._append_log(f"OTA 推送 → {device.host}:{device.port}", "accent")
        self._start_task(worker, self._on_ota_push_done, "正在推送固件 ...")

    def _on_ota_push_done(self, data: dict) -> None:
        error = data.get("error")
        if error:
            self._set_status(f"OTA 推送失败：{error}", theme.ERR)
            QMessageBox.warning(self, "OTA 推送失败", error)
            return
        self.ota_hint.setText("推送完成，设备正在校验并重启 ...")
        self.ota_hint.setStyleSheet(f"color: {theme.OK}; font-size: 11px;")
        self._set_status("OTA 推送完成，设备重启后新固件生效。", theme.OK)

    # ================================================================
    # 窗口事件
    # ================================================================

    def showEvent(self, event) -> None:
        super().showEvent(event)
        if not self._dark_title_done:
            self._dark_title_done = True
            self._apply_dark_titlebar()

    def _apply_dark_titlebar(self) -> None:
        if sys.platform != "win32":
            return
        try:
            hwnd = int(self.winId())
            dwm = ctypes.windll.dwmapi

            def set_attr(attr: int, value: int) -> None:
                data = ctypes.c_int(value)
                dwm.DwmSetWindowAttribute(
                    hwnd, attr, ctypes.byref(data), ctypes.sizeof(data))

            set_attr(20, 1)
            set_attr(34, 0x00241A0E)
            set_attr(35, 0x00140E0B)
            set_attr(36, 0x00F5EDE9)
        except Exception:
            pass

    def closeEvent(self, event) -> None:
        if self._busy:
            QMessageBox.information(
                self, "任务进行中",
                "当前还有任务在运行，请等待完成后再退出。\n"
                "如果正在烧录，请尤其不要中断。")
            event.ignore()
            return
        self._auto_timer.stop()
        if self._tray is not None:
            self._tray.hide()
        self._save_preferences()
        event.accept()
