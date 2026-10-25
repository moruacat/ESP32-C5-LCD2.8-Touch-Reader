# -*- coding: utf-8 -*-
"""图片 → RGB565 RAW 页面。"""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import Qt, QThread, QTimer, QUrl
from PySide6.QtGui import QDesktopServices
from PySide6.QtWidgets import (
    QFileDialog, QHBoxLayout, QLabel, QLineEdit, QPushButton, QSizePolicy,
)

from .. import icons, theme
from ..widgets import (
    Card, Divider, DropZone, IMAGE_SUFFIXES, ImagePreview, LogView, PageHeader,
    ProgressBar, ResultBanner, SelectionBar, Switch, setting_row,
)
from ..workers import PhotoWorker
from .base import BasePage


class PhotoPage(BasePage):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.paths: list[str] = []
        self._thread: QThread | None = None
        self._worker: PhotoWorker | None = None

        self._build()
        self._restore_settings()

    # ---------------- 界面 ----------------

    def _build(self) -> None:
        header = PageHeader(
            "图片 → RGB565 RAW",
            "批量转成 320×240 小端裸数据，直接放进 SD 卡 /photos 目录",
        )
        self.open_btn = QPushButton("打开输出目录")
        self.open_btn.setProperty("variant", "ghost")
        self.open_btn.setIcon(icons.icon("external", theme.MUTED, 15))
        self.open_btn.setCursor(Qt.PointingHandCursor)
        self.open_btn.clicked.connect(self._open_output)
        header.add_action(self.open_btn)
        self.column.addWidget(header)

        self.drop = DropZone(
            "image",
            "拖入图片或文件夹",
            "点击也可以选择图片（可多选）",
            "PNG / JPG / JPEG / WEBP；文件夹会递归扫描，等比缩放后居中贴到黑底",
            IMAGE_SUFFIXES,
        )
        self.drop.browseFiles.connect(self._pick_files)
        self.drop.browseFolder.connect(self._pick_folder)
        self.drop.pathsDropped.connect(self._add_paths)
        self.column.addWidget(self.drop)

        self.selection = SelectionBar()
        self.selection.cleared.connect(self._clear_selection)
        self.column.addWidget(self.selection)

        # 输出位置
        out_card = Card(
            "输出位置",
            "留空时生成在源图片旁边；输出固定 153600 字节（320×240×2），无文件头")
        out_row = QHBoxLayout()
        out_row.setSpacing(8)
        self.out_edit = QLineEdit()
        self.out_edit.setPlaceholderText("留空 = 与源图片同目录")
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

        # 选项 + 预览
        middle = QHBoxLayout()
        middle.setSpacing(16)

        opts = Card("转换选项", "输出 320×240 · RGB565 小端 · 16 bit")
        self.sw_overwrite = Switch(False)
        opts.add(setting_row(
            "覆盖已存在的 .raw",
            "关闭时同名输出自动变成 name_1.raw、name_2.raw …",
            self.sw_overwrite,
        ))
        opts.add(Divider())

        self.sw_rotate = Switch(True)
        opts.add(setting_row(
            "自动旋转",
            "竖图逆时针转 90° 后能显示更多像素才旋转；横图、方图保持原样",
            self.sw_rotate,
        ))
        opts.body.addStretch(1)
        middle.addWidget(opts, 3)

        self.preview = ImagePreview()
        middle.addWidget(self.preview, 2)
        self.column.addLayout(middle)

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

        self.log_view.append_line("准备就绪。选择图片后点击「开始转换」。", "muted")

    # ---------------- 选择 ----------------

    def _pick_files(self) -> None:
        names = self.choose_files(
            "选择图片（可多选）",
            "图片 (*.png *.jpg *.jpeg *.webp);;所有文件 (*.*)")
        if names:
            self._add_paths(names)

    def _pick_folder(self) -> None:
        name = self.choose_dir("选择包含图片的文件夹")
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
        self.preview.set_paths(self.paths, IMAGE_SUFFIXES, self.sw_rotate.isChecked())
        self.banner.setVisible(False)

    def _clear_selection(self) -> None:
        if self._busy:
            return
        self.paths = []
        self.selection.update_paths([])
        self.preview.clear()

    # ---------------- 运行 ----------------

    def start(self) -> None:
        if self._busy:
            return

        if not self.paths:
            self._show_error("还没有选择图片", "把图片或文件夹拖进来，或者点击上方虚线框选择")
            return

        missing = [p for p in self.paths if not Path(p).exists()]
        if len(missing) == len(self.paths):
            self._show_error("路径不存在", missing[0])
            return

        out_text = self.out_edit.text().strip().strip('"')
        out_root: Path | None = Path(out_text) if out_text else None
        if out_root is not None:
            try:
                out_root.mkdir(parents=True, exist_ok=True)
            except OSError as exc:
                self._show_error("输出目录不可用", f"{out_root}：{exc}")
                return

        self.banner.setVisible(False)
        self.log_view.reset()
        self.log_view.append_line(
            f"开始转换 → {out_root or '各图片所在目录'}", "muted")

        self.progress.setRange(0, 0)
        self._set_busy(True)
        QTimer.singleShot(80, self.scroll_to_bottom)

        self._worker = PhotoWorker(
            self.paths,
            out_root,
            overwrite=self.sw_overwrite.isChecked(),
            auto_rotate=self.sw_rotate.isChecked(),
        )
        self._thread = QThread(self)
        self._worker.moveToThread(self._thread)

        self._thread.started.connect(self._worker.run)
        self._worker.log.connect(self.log_view.append_line)
        self._worker.progress.connect(self._on_progress)
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
        self._set_status("正在停止…（当前图片转完后停下）")

    # ---------------- 信号 ----------------

    def _on_progress(self, done: int, total: int, name: str) -> None:
        self.progress.setRange(0, max(1, total))
        self.progress.setValue(done)
        self._set_status(f"{done}/{total} · {name}")

    def _on_done(self, result: dict) -> None:
        self._set_busy(False)

        elapsed = f"{result['elapsed']:.1f} 秒"
        ok = result["ok"]
        fail = result["fail"]
        skipped = result.get("skipped", 0)
        total = result["total"]

        if total:
            self.progress.setRange(0, total)
            self.progress.setValue(total if not result["stopped"] else ok + fail)
        else:
            self.progress.setRange(0, 1)
            self.progress.setValue(0)

        out_dir = self._output_dir()

        if result["error"]:
            self._show_error("没有可转换的图片", result["error"])
            self._set_status("失败", error=True)
            return

        if result["stopped"]:
            self.banner.show_result(
                "warn", f"已停止 · 完成 {ok}/{total}",
                f"成功 {ok}，跳过 {skipped}，失败 {fail}，用时 {elapsed}", out_dir)
            self._set_status(f"已停止：成功 {ok} / 失败 {fail}")
        elif fail:
            self.banner.show_result(
                "warn", f"完成，但有 {fail} 个失败",
                f"成功 {ok}，跳过 {skipped}，失败 {fail}，用时 {elapsed} · 详情见日志", out_dir)
            self._set_status(f"完成：成功 {ok} / 失败 {fail}", error=True)
        else:
            self.banner.show_result(
                "success", f"转换完成 · {ok} 张",
                f"跳过 {skipped} · 用时 {elapsed} · 拷到 SD 卡 /photos/", out_dir)
            self._set_status(f"完成：{ok} 张，用时 {elapsed}")

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
        self.stop_btn.setEnabled(busy)
        self.drop.setEnabled(not busy)
        self.selection.clear_btn.setEnabled(not busy)
        self.sw_overwrite.setEnabled(not busy)
        self.sw_rotate.setEnabled(not busy)
        self.out_edit.setEnabled(not busy)
        self.open_btn.setEnabled(not busy)

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
        for raw in self.paths:
            path = Path(raw)
            if path.is_file():
                return str(path.parent)
            if path.is_dir():
                return str(path)
        return None

    def _open_output(self) -> None:
        target = self._output_dir()
        if not target:
            self._set_status("还没有输出目录可打开")
            return
        QDesktopServices.openUrl(QUrl.fromLocalFile(target))

    # ---------------- 设置持久化 ----------------

    def _restore_settings(self) -> None:
        self.sw_overwrite.setChecked(self.load_bool("photo/overwrite", False))
        self.sw_rotate.setChecked(self.load_bool("photo/autorotate", True))
        self.sw_overwrite.toggled.connect(lambda _: self._save_settings())
        self.sw_rotate.toggled.connect(self._on_rotate_changed)

    def _save_settings(self) -> None:
        self.save_bool("photo/overwrite", self.sw_overwrite.isChecked())
        self.save_bool("photo/autorotate", self.sw_rotate.isChecked())

    def _on_rotate_changed(self, checked: bool) -> None:
        if self.paths:
            self.preview.set_paths(self.paths, IMAGE_SUFFIXES, checked)
        self._save_settings()
