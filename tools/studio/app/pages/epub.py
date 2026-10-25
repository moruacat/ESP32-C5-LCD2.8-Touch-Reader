# -*- coding: utf-8 -*-
"""EPUB → EBF 页面。"""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import Qt, QThread, QTimer, QUrl
from PySide6.QtGui import QDesktopServices
from PySide6.QtWidgets import (
    QFileDialog, QHBoxLayout, QLabel, QLineEdit, QPushButton, QSizePolicy,
)

from .. import icons, theme
from ..widgets import (
    Card, Divider, DropZone, EPUB_SUFFIXES, LogView, NumberStepper,
    PageHeader, ProgressBar, ResultBanner, SelectionBar, Switch, setting_row,
)
from ..workers import EpubWorker, epub2ebf
from .base import BasePage


class EpubPage(BasePage):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.paths: list[str] = []
        self._files: list[Path] = []
        self._thread: QThread | None = None
        self._worker: EpubWorker | None = None

        self._build()
        self._restore_settings()

    # ---------------- 界面 ----------------

    def _build(self) -> None:
        header = PageHeader(
            "EPUB → EBF",
            "把 EPUB 拆成阅读器能直接读取的章节文件夹（chapter.N.ebfl + data.ebfl + cover.raw）",
        )
        self.open_btn = QPushButton("打开输出目录")
        self.open_btn.setProperty("variant", "ghost")
        self.open_btn.setIcon(icons.icon("external", theme.MUTED, 15))
        self.open_btn.setCursor(Qt.PointingHandCursor)
        self.open_btn.clicked.connect(self._open_output)
        header.add_action(self.open_btn)
        self.column.addWidget(header)

        self.drop = DropZone(
            "book",
            "拖入 EPUB 文件或文件夹",
            "点击也可以选择文件",
            "支持多选与递归扫描；一本书会生成一个 <书名>.ebf 文件夹",
            EPUB_SUFFIXES,
        )
        self.drop.browseFiles.connect(self._pick_files)
        self.drop.browseFolder.connect(self._pick_folder)
        self.drop.pathsDropped.connect(self._add_paths)
        self.column.addWidget(self.drop)

        self.selection = SelectionBar()
        self.selection.cleared.connect(self._clear_selection)
        self.column.addWidget(self.selection)

        # 输出位置
        out_card = Card("输出位置", "留空时生成在 EPUB 旁边；批量选择文件夹时生成在该文件夹里")
        out_row = QHBoxLayout()
        out_row.setSpacing(8)
        self.out_edit = QLineEdit()
        self.out_edit.setPlaceholderText("留空 = 与源文件同目录")
        self.out_edit.setMinimumHeight(36)
        self.out_edit.setClearButtonEnabled(True)
        btn_out = QPushButton("浏览")
        btn_out.setCursor(Qt.PointingHandCursor)
        btn_out.clicked.connect(self._pick_output)
        btn_reset = QPushButton("恢复默认")
        btn_reset.setProperty("variant", "ghost")
        btn_reset.setCursor(Qt.PointingHandCursor)
        btn_reset.clicked.connect(lambda: self.out_edit.clear())
        out_row.addWidget(self.out_edit, 1)
        out_row.addWidget(btn_out)
        out_row.addWidget(btn_reset)
        out_card.add_layout(out_row)
        self.column.addWidget(out_card)

        # 选项
        opts = Card("转换选项")
        self.skip_stepper = NumberStepper(0, 99, 0)
        opts.add(setting_row(
            "跳过前 N 章",
            "用来丢掉简介、前言、目录页；被跳过的章名会打印在日志里",
            self.skip_stepper,
        ))
        opts.add(Divider())

        self.sw_force = Switch(True)
        opts.add(setting_row(
            "覆盖已存在的输出目录",
            "关闭时，若 <书名>.ebf 已存在会直接报错",
            self.sw_force,
        ))
        opts.add(Divider())

        self.sw_blank = Switch(False)
        opts.add(setting_row(
            "段落之间空一行",
            "默认每段一行、段间不留空行，小屏一页能放更多字",
            self.sw_blank,
        ))
        opts.add(Divider())

        self.sw_dedup = Switch(False)
        opts.add(setting_row(
            "去掉正文开头重复的章标题",
            "EPUB 正文里常带一个和目录重复的 <h1>，勾上后删掉第一行",
            self.sw_dedup,
        ))
        self.column.addWidget(opts)

        # 运行行
        run_row = QHBoxLayout()
        run_row.setSpacing(10)
        self.start_btn = QPushButton("开始转换")
        self.start_btn.setProperty("variant", "primary")
        self.start_btn.setIcon(icons.icon("play", "#FFFFFF", 15))
        self.start_btn.setCursor(Qt.PointingHandCursor)
        self.start_btn.setMinimumWidth(148)
        self.start_btn.clicked.connect(self.start)

        self.stop_btn = QPushButton("停止")
        self.stop_btn.setIcon(icons.icon("stop", theme.MUTED, 13))
        self.stop_btn.setCursor(Qt.PointingHandCursor)
        self.stop_btn.setEnabled(False)
        self.stop_btn.clicked.connect(self.request_stop)

        self.status_label = QLabel("就绪")
        self.status_label.setTextFormat(Qt.PlainText)
        self.status_label.setStyleSheet(f"color: {theme.MUTED}; font-size: 12px;")
        self.status_label.setMaximumWidth(380)
        self.status_label.setSizePolicy(QSizePolicy.Maximum, QSizePolicy.Preferred)

        run_row.addWidget(self.start_btn)
        run_row.addWidget(self.stop_btn)
        run_row.addStretch(1)
        run_row.addWidget(self.status_label)
        self.column.addLayout(run_row)

        self.progress = ProgressBar()
        self.column.addWidget(self.progress)

        self.banner = ResultBanner()
        self.column.addWidget(self.banner)

        log_card = Card("日志")
        clear_btn = QPushButton("清空")
        clear_btn.setProperty("variant", "ghost")
        clear_btn.setCursor(Qt.PointingHandCursor)
        clear_btn.clicked.connect(lambda: self.log_view.reset())
        log_card.header.addWidget(clear_btn)

        self.log_view = LogView()
        log_card.add(self.log_view)
        self.column.addWidget(log_card, 1)

        self.log_view.append_line("准备就绪。选择 EPUB 后点击「开始转换」。", "muted")

    # ---------------- 选择 ----------------

    def _pick_files(self) -> None:
        names = self.choose_files("选择 EPUB", "EPUB 电子书 (*.epub);;所有文件 (*.*)")
        if names:
            self._add_paths(names)

    def _pick_folder(self) -> None:
        name = self.choose_dir("选择包含 EPUB 的文件夹")
        if name:
            self._add_paths([name])

    def _pick_output(self) -> None:
        start = self.out_edit.text().strip() or (
            str(Path(self.paths[0]).parent) if self.paths else "")
        name = QFileDialog.getExistingDirectory(self, "选择输出目录", start)
        if name:
            self.out_edit.setText(name)

    def _add_paths(self, paths: list[str]) -> None:
        if self._busy:
            return
        merged: list[str] = []
        seen: set[str] = set()
        for raw in self.paths + paths:
            key = raw.lower()
            if key not in seen:
                seen.add(key)
                merged.append(raw)
        self.paths = merged
        self.selection.update_paths(self.paths)
        self.banner.setVisible(False)

    def _clear_selection(self) -> None:
        if self._busy:
            return
        self.paths = []
        self.selection.update_paths([])

    # ---------------- 运行 ----------------

    def start(self) -> None:
        if self._busy:
            return

        if not self.paths:
            self._show_error("还没有选择 EPUB", "把一个 .epub 文件或包含 EPUB 的文件夹拖进来")
            return

        warn_buffer: list[str] = []
        files = epub2ebf.collect_inputs(self.paths, warn=warn_buffer.append)
        if not files:
            detail = warn_buffer[0] if warn_buffer else "目录里没有 .epub 文件"
            self._show_error("没有找到 EPUB 文件", detail)
            return

        out_text = self.out_edit.text().strip().strip('"')
        if out_text:
            out_root: Path | None = Path(out_text)
        elif len(self.paths) == 1 and Path(self.paths[0]).is_dir():
            out_root = Path(self.paths[0])
        else:
            out_root = None

        if out_root is not None:
            try:
                out_root.mkdir(parents=True, exist_ok=True)
            except OSError as exc:
                self._show_error("输出目录不可用", f"{out_root}：{exc}")
                return

        self._files = files
        self.banner.setVisible(False)
        self.log_view.reset()
        self.log_view.append_line(
            f"开始转换 {len(files)} 个文件 → {out_root or '各 EPUB 所在目录'}", "muted")

        for msg in warn_buffer:
            self.log_view.append_line(msg, "warn")

        self.progress.setMaximum(max(1, len(files)))
        self.progress.setValue(0)
        self._set_busy(True)
        QTimer.singleShot(80, self.scroll_to_bottom)

        self._worker = EpubWorker(
            files,
            out_root,
            force=self.sw_force.isChecked(),
            blank_line=self.sw_blank.isChecked(),
            skip=self.skip_stepper.value(),
            dedup_title=self.sw_dedup.isChecked(),
        )
        self._thread = QThread(self)
        self._worker.moveToThread(self._thread)

        self._thread.started.connect(self._worker.run)
        self._worker.log.connect(self.log_view.append_line)
        self._worker.progress.connect(self._on_progress)
        self._worker.chapter.connect(self._on_chapter)
        self._worker.done.connect(self._on_done)
        self._worker.done.connect(self._thread.quit)
        self._thread.finished.connect(self._worker.deleteLater)
        self._thread.finished.connect(self._on_thread_finished)

        self._thread.start()

    def request_stop(self) -> None:
        if not self._busy or self._worker is None:
            return
        self._worker.stop()
        self.stop_btn.setEnabled(False)
        self._set_status("正在停止…（当前书籍转换完成后停下）")

    # ---------------- 信号 ----------------

    def _on_progress(self, done: int, total: int, name: str) -> None:
        self.progress.setMaximum(max(1, total))
        self.progress.setValue(done)
        self._set_status(f"{done}/{total} · {name}")

    def _on_chapter(self, done: int, total: int, msg: str) -> None:
        if len(self._files) != 1 or total <= 0:
            return
        self.progress.setMaximum(total)
        self.progress.setValue(min(total, done + 1))
        if msg:
            self._set_status(msg)

    def _on_done(self, result: dict) -> None:
        self._set_busy(False)
        self.progress.setValue(self.progress.maximum())

        elapsed = f"{result['elapsed']:.1f} 秒"
        ok = result["ok"]
        fail = result["fail"]
        total = result["total"]
        out_dir = self._output_dir()

        if result["error"]:
            self._show_error("转换失败", result["error"])
            self._set_status("失败", error=True)
            return

        if result["stopped"]:
            self.banner.show_result(
                "warn", f"已停止 · 完成 {ok}/{total}",
                f"成功 {ok}，失败 {fail}，用时 {elapsed}", out_dir)
            self._set_status(f"已停止：成功 {ok} / 失败 {fail}")
        elif fail:
            self.banner.show_result(
                "warn", f"完成，但有 {fail} 个失败",
                f"成功 {ok}，失败 {fail}，用时 {elapsed} · 详情见日志", out_dir)
            self._set_status(f"完成：成功 {ok} / 失败 {fail}", error=True)
        else:
            self.banner.show_result(
                "success", f"转换完成 · {ok} 本书",
                f"用时 {elapsed} · 把 .ebf 文件夹拷到 SD 卡 /books/", out_dir)
            self._set_status(f"完成：{ok} 本，用时 {elapsed}")

    def _on_thread_finished(self) -> None:
        thread = self._thread
        self._worker = None
        self._thread = None
        if thread is not None:
            thread.deleteLater()

    # ---------------- 状态 ----------------

    def _set_busy(self, busy: bool) -> None:
        self._busy = busy
        self.start_btn.setEnabled(not busy)
        self.stop_btn.setEnabled(busy and len(self._files) > 1)
        self.drop.setEnabled(not busy)
        self.selection.clear_btn.setEnabled(not busy)
        self.skip_stepper.setEnabled(not busy)
        self.sw_force.setEnabled(not busy)
        self.sw_blank.setEnabled(not busy)
        self.sw_dedup.setEnabled(not busy)
        self.out_edit.setEnabled(not busy)
        if busy:
            self.open_btn.setEnabled(False)
        else:
            self.open_btn.setEnabled(True)

    def _set_status(self, text: str, error: bool = False) -> None:
        color = theme.ERR if error else theme.MUTED
        self.status_label.setStyleSheet(f"color: {color}; font-size: 12px;")
        metrics = self.status_label.fontMetrics()
        shown = metrics.elidedText(text, Qt.ElideMiddle, 360)
        self.status_label.setText(shown)
        self.status_label.setToolTip(text)

    def _show_error(self, title: str, detail: str) -> None:
        self.banner.show_result("error", title, detail)
        self._set_status(title, error=True)

    def _output_dir(self) -> str | None:
        out_text = self.out_edit.text().strip().strip('"')
        if out_text:
            return out_text
        if self._files:
            return str(self._files[0].parent)
        return None

    def _open_output(self) -> None:
        target = self._output_dir()
        if not target:
            self._set_status("还没有输出目录可打开")
            return
        QDesktopServices.openUrl(QUrl.fromLocalFile(target))

    # ---------------- 设置持久化 ----------------

    def _restore_settings(self) -> None:
        self.skip_stepper.setValue(self.load_int("epub/skip", 0))
        self.sw_force.setChecked(self.load_bool("epub/force", True))
        self.sw_blank.setChecked(self.load_bool("epub/blank", False))
        self.sw_dedup.setChecked(self.load_bool("epub/dedup", False))

        self.skip_stepper.valueChanged.connect(lambda _: self._save_settings())
        self.sw_force.toggled.connect(lambda _: self._save_settings())
        self.sw_blank.toggled.connect(lambda _: self._save_settings())
        self.sw_dedup.toggled.connect(lambda _: self._save_settings())

    def _save_settings(self) -> None:
        self.save_bool("epub/force", self.sw_force.isChecked())
        self.save_bool("epub/blank", self.sw_blank.isChecked())
        self.save_bool("epub/dedup", self.sw_dedup.isChecked())
        self.settings.setValue("epub/skip", self.skip_stepper.value())
