# -*- coding: utf-8 -*-
"""把两个转换器包成后台任务。

转换本身仍然是 tools/epub2ebf 与 tools/photo2raw 里的函数，这里只负责：
    - 在 QThread 里跑，别卡界面
    - 把回调转成 Qt 信号（跨线程自动排队回主线程）
    - 支持在文件之间“停止”
"""

from __future__ import annotations

import sys
import threading
import time
from pathlib import Path

from PySide6.QtCore import QObject, Signal

TOOLS_DIR = Path(__file__).resolve().parents[2]
for _sub in ("epub2ebf", "photo2raw"):
    _path = str(TOOLS_DIR / _sub)
    if _path not in sys.path:
        sys.path.insert(0, _path)

import epub2ebf                                            # noqa: E402
import photo2raw                                           # noqa: E402


class BaseWorker(QObject):
    log = Signal(str, str)                # 文本, 级别
    progress = Signal(int, int, str)      # 已完成, 总数, 说明
    done = Signal(dict)

    def __init__(self) -> None:
        super().__init__()
        self._stop = threading.Event()

    def stop(self) -> None:
        """主线程调用：置位后，当前文件做完就停。"""
        self._stop.set()

    def _stopped(self) -> bool:
        return self._stop.is_set()

    def _level(self, msg: str) -> str:
        return "error" if msg.startswith("[失败]") else "warn"


class EpubWorker(BaseWorker):
    chapter = Signal(int, int, str)       # 当前书的章节进度

    def __init__(self, files: list[Path], out_root: Path | None,
                 force: bool, blank_line: bool, skip: int, dedup_title: bool):
        super().__init__()
        self.files = files
        self.out_root = out_root
        self.force = force
        self.blank_line = blank_line
        self.skip = skip
        self.dedup_title = dedup_title

    def run(self) -> None:
        started = time.perf_counter()
        ok = fail = 0
        total = len(self.files)
        stopped = False

        opt = epub2ebf.Options(
            out_root=self.out_root,
            force=self.force,
            blank_line=self.blank_line,
            skip=max(0, self.skip),
            dedup_title=self.dedup_title,
            log=lambda m: self.log.emit(m, "info"),
            warn=lambda m: self.log.emit(m, self._level(m)),
            progress=lambda d, t, m: self.chapter.emit(d, t, m),
        )

        try:
            for i, src in enumerate(self.files):
                if self._stopped():
                    stopped = True
                    break
                if total > 1:
                    self.log.emit(f"\n[{i + 1}/{total}] {src.name}", "info_accent")
                else:
                    self.log.emit(f"{src.name}", "info_accent")

                if epub2ebf.convert_one(src, opt):
                    ok += 1
                else:
                    fail += 1
                self.progress.emit(i + 1, total, src.name)

            if stopped:
                self.log.emit("已停止：当前书籍转换完成后中断。", "warn")
        except Exception as exc:                           # 理论上到不了，兜底
            self.log.emit(f"[失败] {type(exc).__name__}: {exc}", "error")
            fail = max(fail, 1)
        finally:
            self.done.emit({
                "ok": ok,
                "fail": fail,
                "total": total,
                "stopped": stopped,
                "elapsed": time.perf_counter() - started,
                "error": None,
            })


class PhotoWorker(BaseWorker):
    def __init__(self, paths: list[str], out_root: Path | None, overwrite: bool,
                 auto_rotate: bool = False):
        super().__init__()
        self.paths = paths
        self.out_root = out_root
        self.overwrite = overwrite
        self.auto_rotate = auto_rotate

    def run(self) -> None:
        started = time.perf_counter()
        ok = fail = skipped = 0
        stopped = False
        total = 0
        error: str | None = None

        opt = photo2raw.Options(
            out_root=self.out_root,
            overwrite=self.overwrite,
            auto_rotate=self.auto_rotate,
            log=lambda m: self.log.emit(m, "info"),
            warn=lambda m: self.log.emit(m, self._level(m)),
        )

        try:
            files, skipped = photo2raw.collect_inputs(
                self.paths, warn=lambda m: self.log.emit(m, "muted"))
            total = len(files)

            if not files:
                error = "没有找到可转换的图片"
            else:
                if self.out_root is not None:
                    self.out_root.mkdir(parents=True, exist_ok=True)

                for i, src in enumerate(files):
                    if self._stopped():
                        stopped = True
                        break
                    if total > 1:
                        self.log.emit(f"\n[{i + 1}/{total}] {src.name}", "info_accent")
                    else:
                        self.log.emit(f"{src.name}", "info_accent")

                    if photo2raw.convert_one(src, opt) == photo2raw.STATUS_OK:
                        ok += 1
                    else:
                        fail += 1
                    self.progress.emit(i + 1, total, src.name)

                if stopped:
                    self.log.emit("已停止：当前图片转换完成后中断。", "warn")
        except Exception as exc:
            self.log.emit(f"[失败] {type(exc).__name__}: {exc}", "error")
            error = f"{type(exc).__name__}: {exc}"
        finally:
            self.done.emit({
                "ok": ok,
                "fail": fail,
                "skipped": skipped,
                "total": total,
                "stopped": stopped,
                "elapsed": time.perf_counter() - started,
                "error": error,
            })
