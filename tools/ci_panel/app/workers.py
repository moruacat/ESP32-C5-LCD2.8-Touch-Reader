# -*- coding: utf-8 -*-
"""后台任务：查询构建、下载 artifact、下载日志、烧录。

所有阻塞操作都放在 QThread 里执行；本文件只通过 Qt Signal 回主线程，
不直接碰窗口控件。
"""

from __future__ import annotations

import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
import zipfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Callable

from PySide6.QtCore import QObject, Signal

from .esptool_util import EsptoolTool
from .github_api import GitHubClient, GitHubError, USER_AGENT


class WorkerCancelled(RuntimeError):
    pass


class BaseWorker(QObject):
    log = Signal(str, str)                 # 文本, 级别
    progress = Signal(int, int, str)       # 已完成, 总数（0 表示不确定）, 说明
    done = Signal(dict)

    def __init__(self) -> None:
        super().__init__()
        self._cancel = threading.Event()

    def cancel(self) -> None:
        self._cancel.set()

    def _check_cancel(self) -> None:
        if self._cancel.is_set():
            raise WorkerCancelled("操作已取消")

    def _fail(self, message: str) -> None:
        self.log.emit(f"[失败] {message}", "error")
        self.done.emit({"error": message})


class RunsWorker(BaseWorker):
    def __init__(self, repo: str, branch: str, token: str) -> None:
        super().__init__()
        self.repo = repo
        self.branch = branch
        self.token = token

    def run(self) -> None:
        self.progress.emit(0, 0, "正在查询 GitHub Actions 构建...")
        try:
            client = GitHubClient(self.repo, self.token)
            runs = client.list_runs(self.branch, 30)
            self.done.emit({"runs": runs, "error": None})
        except Exception as exc:
            self._fail(str(exc))


class GitPushWorker(BaseWorker):
    """把当前本地分支推送到 origin。

    只执行 git push，不会自动 add/commit；未提交内容不会被推送。
    """

    def __init__(self, repo_path: Path, branch: str) -> None:
        super().__init__()
        self.repo_path = Path(repo_path)
        self.branch = branch.strip()

    def run(self) -> None:
        started = time.perf_counter()
        cmd = ["git", "-C", str(self.repo_path), "push", "origin", self.branch]
        self.progress.emit(0, 0, f"正在推送 {self.branch} 到 origin...")
        self.log.emit("执行：" + " ".join(cmd), "muted")

        env = os.environ.copy()
        env["GIT_TERMINAL_PROMPT"] = "0"
        env["GCM_INTERACTIVE"] = "Never"
        creationflags = 0
        if sys.platform == "win32":
            creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)

        try:
            proc = subprocess.Popen(
                cmd,
                cwd=str(self.repo_path),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
                env=env,
                creationflags=creationflags,
            )
            assert proc.stdout is not None
            for line in proc.stdout:
                self.log.emit(line.rstrip(), "info")
                if self._cancel.is_set():
                    proc.terminate()
                    break
            code = proc.wait()
            if self._cancel.is_set():
                raise WorkerCancelled("推送已取消")
            if code != 0:
                raise RuntimeError(
                    f"git push 失败（退出码 {code}）。请确认本机 Git 凭据、远程地址和分支权限。")

            elapsed = time.perf_counter() - started
            self.log.emit(f"推送完成：{self.branch} -> origin", "ok")
            self.done.emit({
                "error": None,
                "branch": self.branch,
                "elapsed": elapsed,
            })
        except WorkerCancelled as exc:
            self._fail(str(exc))
        except FileNotFoundError as exc:
            self._fail(f"找不到 git 命令：{exc}")
        except Exception as exc:
            self._fail(str(exc))


class ArtifactWorker(BaseWorker):
    """下载最新指定 run 的 firmware artifact，并解压到 build/ci/run-<id>。"""

    def __init__(self, repo: str, token: str, run_id: int, head_sha: str,
                 out_root: Path, threads: int = 8) -> None:
        super().__init__()
        self.repo = repo
        self.token = token
        self.run_id = run_id
        self.head_sha = head_sha
        self.out_root = out_root
        self.threads = max(1, int(threads))

        self._progress_lock = threading.Lock()
        self._progress_done = 0
        self._progress_total = 0
        self._progress_last = 0.0
        self._progress_message = ""

    # ---------------- 进度 ----------------

    def _reset_progress(self, total: int, message: str) -> None:
        with self._progress_lock:
            self._progress_done = 0
            self._progress_total = max(0, int(total))
            self._progress_last = 0.0
            self._progress_message = message
        self.progress.emit(0, max(0, int(total)), message)

    def _add_progress(self, count: int, message: str = "") -> None:
        emit = False
        values = (0, 0, "")
        with self._progress_lock:
            self._progress_done += max(0, int(count))
            if message:
                self._progress_message = message
            now = time.monotonic()
            total = self._progress_total
            finished = total > 0 and self._progress_done >= total
            if finished or now - self._progress_last >= 0.12:
                self._progress_last = now
                values = (self._progress_done, total, self._progress_message)
                emit = True
        if emit:
            self.progress.emit(*values)

    # ---------------- 生命周期 ----------------

    def run(self) -> None:
        started = time.perf_counter()
        tmp_dir = Path(tempfile.mkdtemp(prefix="morua-ci-fw-"))
        out_dir = self.out_root / f"run-{self.run_id}"
        try:
            client = GitHubClient(self.repo, self.token)
            self.log.emit(f"查询运行 #{self.run_id} 的 firmware artifact ...", "info")
            artifact = client.find_firmware_artifact(self.run_id)
            if artifact.expired:
                raise GitHubError("该 artifact 已过期，请选择更新的成功构建。")
            self.log.emit(
                f"找到 artifact：{artifact.name}，{artifact.size_in_bytes:,} 字节",
                "ok",
            )

            self.progress.emit(0, 0, "正在解析临时下载地址...")
            download_url = client.resolve_artifact_url(artifact.id)
            self.log.emit("已获取临时下载地址，开始下载 ...", "muted")

            zip_path = tmp_dir / "firmware.zip"
            total = self._download(download_url, zip_path, artifact.size_in_bytes)
            self._check_cancel()

            if not zipfile.is_zipfile(zip_path):
                raise GitHubError("下载到的 firmware 不是有效的 zip 文件。")

            if out_dir.exists():
                shutil.rmtree(out_dir)
            out_dir.mkdir(parents=True, exist_ok=True)

            self.progress.emit(0, 0, "正在解压 firmware ...")
            self._extract_zip(zip_path, out_dir)
            self._check_cancel()

            app = self._find_one(out_dir, "clock_reader.bin", required=True)
            boot = self._find_one(out_dir, "bootloader.bin", required=True)
            part = self._find_one(out_dir, "partition-table.bin", required=True)
            ota = self._find_one(out_dir, "ota_data_initial.bin", required=False)
            assert app is not None and boot is not None and part is not None

            meta = {
                "repo": self.repo,
                "run_id": self.run_id,
                "head_sha": self.head_sha,
                "artifact_id": artifact.id,
                "artifact_name": artifact.name,
                "downloaded_at": time.strftime("%Y-%m-%d %H:%M:%S"),
                "app": str(app),
                "bootloader": str(boot),
                "partition_table": str(part),
                "ota_data": str(ota) if ota else "",
            }
            (out_dir / "download-meta.json").write_text(
                json.dumps(meta, ensure_ascii=False, indent=2),
                encoding="utf-8",
            )

            elapsed = time.perf_counter() - started
            self.log.emit(f"固件已解压到：{out_dir}", "ok")
            self.progress.emit(total, total, "下载完成")
            self.done.emit({
                "error": None,
                "run_id": self.run_id,
                "head_sha": self.head_sha,
                "artifact_id": artifact.id,
                "out_dir": str(out_dir),
                "app": str(app),
                "bootloader": str(boot),
                "partition_table": str(part),
                "ota_data": str(ota) if ota else "",
                "total_bytes": total,
                "elapsed": elapsed,
            })
        except WorkerCancelled as exc:
            self._fail(str(exc))
        except Exception as exc:
            self._fail(str(exc))
        finally:
            shutil.rmtree(tmp_dir, ignore_errors=True)

    # ---------------- 下载 ----------------

    def _probe_range(self, url: str, expected_size: int) -> tuple[bool, int]:
        req = urllib.request.Request(url, headers={
            "User-Agent": USER_AGENT,
            "Range": "bytes=0-0",
        })
        try:
            with urllib.request.urlopen(req, timeout=45) as resp:
                content_range = resp.headers.get("Content-Range") or ""
                total = 0
                match = re.search(r"/(\d+)\s*$", content_range)
                if match:
                    total = int(match.group(1))
                if total <= 0:
                    total = int(resp.headers.get("Content-Length") or expected_size or 0)
                return resp.status == 206 and total > 0, total
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            raise GitHubError(f"下载地址探测失败：{exc}") from exc

    def _download(self, url: str, zip_path: Path, expected_size: int) -> int:
        try:
            range_ok, total = self._probe_range(url, expected_size)
        except GitHubError as exc:
            self.log.emit(f"{exc}，改用单连接下载。", "warn")
            range_ok, total = False, expected_size
        if range_ok:
            try:
                self._parallel_download(url, total, zip_path)
                if zipfile.is_zipfile(zip_path):
                    return total
                self.log.emit("分段下载结果校验失败，改用单连接重试。", "warn")
                zip_path.unlink(missing_ok=True)
            except _RangeFallback as exc:
                self.log.emit(f"{exc}，改用单连接下载。", "warn")
                zip_path.unlink(missing_ok=True)

        total = self._single_download(url, zip_path, expected_size or total)
        if not zipfile.is_zipfile(zip_path):
            raise GitHubError("固件 zip 校验失败，请重试。")
        return total

    def _single_download(self, url: str, zip_path: Path, expected_size: int) -> int:
        self.log.emit("使用单连接下载 ...", "muted")
        req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
        try:
            with urllib.request.urlopen(req, timeout=120) as resp:
                total = int(resp.headers.get("Content-Length") or expected_size or 0)
                self._reset_progress(total, "下载 firmware ...")
                with zip_path.open("wb") as fh:
                    while True:
                        self._check_cancel()
                        chunk = resp.read(256 * 1024)
                        if not chunk:
                            break
                        fh.write(chunk)
                        self._add_progress(len(chunk))
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            raise GitHubError(f"下载 firmware 失败：{exc}") from exc
        actual = zip_path.stat().st_size
        if total > 0 and actual != total:
            raise GitHubError(f"下载不完整：{actual:,} / {total:,} 字节")
        return total or actual

    def _parallel_download(self, url: str, total: int, zip_path: Path) -> None:
        self._reset_progress(total, "分段下载 firmware ...")
        workers = min(self.threads, max(1, total))
        chunk = int(math.ceil(total / workers))
        parts: list[Path] = []
        stop = threading.Event()
        self.log.emit(f"使用 {workers} 个线程分段下载 {total:,} 字节 ...", "muted")

        def fetch(index: int, start: int, end: int) -> Path:
            part = zip_path.parent / f"firmware.part{index:02d}"
            req = urllib.request.Request(url, headers={
                "User-Agent": USER_AGENT,
                "Range": f"bytes={start}-{end}",
            })
            try:
                with urllib.request.urlopen(req, timeout=120) as resp:
                    if resp.status != 206:
                        raise _RangeFallback("服务器未按范围返回数据")
                    with part.open("wb") as fh:
                        while True:
                            if stop.is_set():
                                raise WorkerCancelled("操作已取消")
                            self._check_cancel()
                            data = resp.read(128 * 1024)
                            if not data:
                                break
                            fh.write(data)
                            self._add_progress(len(data))
            except urllib.error.HTTPError as exc:
                raise _RangeFallback(f"分段请求返回 HTTP {exc.code}") from exc
            except (urllib.error.URLError, TimeoutError, OSError) as exc:
                raise GitHubError(f"分段下载失败：{exc}") from exc
            expected = end - start + 1
            actual = part.stat().st_size
            if actual != expected:
                raise _RangeFallback("分段大小不一致")
            return part

        futures = []
        try:
            with ThreadPoolExecutor(max_workers=workers) as pool:
                for i in range(workers):
                    start = i * chunk
                    if start >= total:
                        break
                    end = min(start + chunk - 1, total - 1)
                    futures.append(pool.submit(fetch, i, start, end))
                for future in as_completed(futures):
                    try:
                        parts.append(future.result())
                    except Exception:
                        stop.set()
                        raise
        except WorkerCancelled:
            raise
        except Exception as exc:
            if isinstance(exc, _RangeFallback):
                raise
            if isinstance(exc, GitHubError):
                raise
            raise _RangeFallback(str(exc)) from exc

        if sum(p.stat().st_size for p in parts) != total:
            raise _RangeFallback("分段总大小与 artifact 不一致")

        with zip_path.open("wb") as out:
            for part in sorted(parts):
                with part.open("rb") as src:
                    shutil.copyfileobj(src, out)
        for part in parts:
            part.unlink(missing_ok=True)
        self.progress.emit(total, total, "下载完成")

    # ---------------- 解压与查找 ----------------

    @staticmethod
    def _extract_zip(zip_path: Path, dest: Path) -> None:
        base = dest.resolve()
        with zipfile.ZipFile(zip_path) as zf:
            for info in zf.infolist():
                target = (dest / info.filename).resolve()
                if target != base and base not in target.parents:
                    raise GitHubError("artifact 内含非法路径，已停止解压。")
            zf.extractall(dest)

    @staticmethod
    def _find_one(root: Path, name: str, required: bool) -> Path | None:
        matches = sorted(root.rglob(name), key=lambda p: len(p.parts))
        if matches:
            return matches[0]
        if required:
            raise GitHubError(f"固件包缺少必要文件：{name}")
        return None


class _RangeFallback(RuntimeError):
    pass


class LogsWorker(BaseWorker):
    MAX_LOG_CHARS = 1_500_000

    def __init__(self, repo: str, token: str, run_id: int, title: str) -> None:
        super().__init__()
        self.repo = repo
        self.token = token
        self.run_id = run_id
        self.title = title

    def run(self) -> None:
        fd, name = tempfile.mkstemp(prefix="morua-ci-log-", suffix=".zip")
        os.close(fd)
        tmp = Path(name)
        try:
            client = GitHubClient(self.repo, self.token)
            self.progress.emit(0, 0, "正在解析日志下载地址...")
            url = client.resolve_logs_url(self.run_id)
            self._download(url, tmp)

            self.progress.emit(0, 0, "正在整理日志...")
            text_parts: list[str] = []
            with zipfile.ZipFile(tmp) as zf:
                names = sorted(n for n in zf.namelist()
                               if not n.endswith("/") and not n.startswith("__MACOSX/"))
                for name in names:
                    raw = zf.read(name)
                    text = raw.decode("utf-8", errors="replace")
                    text = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", text)
                    text_parts.append(f"===== {name} =====\n{text.strip()}")

            text = "\n\n".join(text_parts).strip()
            if not text:
                raise GitHubError("日志包是空的。")
            if len(text) > self.MAX_LOG_CHARS:
                removed = len(text) - self.MAX_LOG_CHARS
                text = (f"[日志过长，已省略前 {removed:,} 个字符，以下为末尾内容]\n\n"
                        + text[-self.MAX_LOG_CHARS:])
            self.done.emit({"error": None, "title": self.title, "text": text})
        except WorkerCancelled as exc:
            self._fail(str(exc))
        except Exception as exc:
            self._fail(str(exc))
        finally:
            tmp.unlink(missing_ok=True)

    def _download(self, url: str, path: Path) -> None:
        req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
        try:
            with urllib.request.urlopen(req, timeout=120) as resp:
                total = int(resp.headers.get("Content-Length") or 0)
                self.progress.emit(0, total, "下载日志 ...")
                done = 0
                with path.open("wb") as fh:
                    while True:
                        self._check_cancel()
                        chunk = resp.read(128 * 1024)
                        if not chunk:
                            break
                        fh.write(chunk)
                        done += len(chunk)
                        self.progress.emit(done, total, "下载日志 ...")
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            raise GitHubError(f"下载运行日志失败：{exc}") from exc


class FlashWorker(BaseWorker):
    def __init__(self, tool: EsptoolTool, port: str, paths: dict[str, str],
                 baud: int = 460800) -> None:
        super().__init__()
        self.tool = tool
        self.port = port
        self.paths = paths
        self.baud = baud
        self._proc: subprocess.Popen | None = None

    def cancel(self) -> None:
        super().cancel()
        proc = self._proc
        if proc is not None and proc.poll() is None:
            try:
                proc.terminate()
            except Exception:
                pass

    def run(self) -> None:
        started = time.perf_counter()
        args = [
            "--chip", "esp32c5", "-p", self.port, "-b", str(self.baud),
            "--before", "default-reset", "--after", "hard-reset",
            "write-flash", "--flash-mode", "dio", "--flash-size", "32MB",
            "--flash-freq", "80m",
            "0x2000", self.paths["bootloader"],
            "0x8000", self.paths["partition_table"],
            "0x20000", self.paths["app"],
        ]
        if self.paths.get("ota_data"):
            args[args.index("0x20000"):args.index("0x20000")] = [
                "0xf000", self.paths["ota_data"]]
        cmd = list(self.tool.cmd) + args

        self.log.emit(f"烧录引擎：{self.tool.label}", "muted")
        self.log.emit(f"串口：{self.port}    波特率：{self.baud}", "info")
        self.progress.emit(0, 0, "正在烧录，请勿断开设备 ...")
        creationflags = 0
        if sys.platform == "win32":
            creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        try:
            self._proc = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
                cwd=str(Path(self.paths["app"]).parent),
                creationflags=creationflags,
            )
            assert self._proc.stdout is not None
            for line in self._proc.stdout:
                self.log.emit(line.rstrip(), "info")
                if self._cancel.is_set():
                    self._proc.terminate()
                    break
            code = self._proc.wait()
            if self._cancel.is_set():
                raise WorkerCancelled("烧录已取消")
            if code != 0:
                raise RuntimeError(f"esptool 返回退出码 {code}")
            elapsed = time.perf_counter() - started
            self.log.emit(f"烧录完成，用时 {elapsed:.1f} 秒。", "ok")
            self.done.emit({"error": None, "elapsed": elapsed})
        except WorkerCancelled as exc:
            self._fail(str(exc))
        except Exception as exc:
            self._fail(str(exc))


# ======================================================================
# OTA（局域网推送，方案见 docs/ota_eval.md）
# ======================================================================

class OtaScanWorker(BaseWorker):
    """扫描局域网里开了 OTA 服务的 ClockReader。"""

    def run(self) -> None:
        from .discovery import scan        # zeroconf 延迟导入，没装也能跑别的功能
        self.progress.emit(0, 0, "正在扫描局域网内的设备 ...")
        devices, error = scan()
        if error:
            self._fail(error)
            return
        self.done.emit({"devices": devices, "error": None})


class OtaPushWorker(BaseWorker):
    """把本地固件推给设备：先 GET /api/info 探活，再 POST /ota 分块上传。

    上传用 http.client 边读边发，不把整个 4 MB 固件读进内存。
    设备端接口约定见 docs/ota_eval.md。
    """

    CHUNK = 64 * 1024

    def __init__(self, host: str, port: int, firmware: Path, token: str = "") -> None:
        super().__init__()
        self.host = host
        self.port = int(port)
        self.firmware = Path(firmware)
        self.token = token

    def _probe(self) -> dict:
        """GET /api/info：确认设备在线，顺便拿到版本/内存信息。"""
        import http.client

        conn = http.client.HTTPConnection(self.host, self.port, timeout=5)
        try:
            conn.request("GET", "/api/info")
            resp = conn.getresponse()
            body = resp.read()
            if resp.status != 200:
                raise RuntimeError(f"设备返回 HTTP {resp.status}")
            return json.loads(body.decode("utf-8", "replace") or "{}")
        finally:
            conn.close()

    def run(self) -> None:
        if not self.firmware.is_file():
            self._fail(f"找不到固件：{self.firmware}")
            return

        size = self.firmware.stat().st_size
        self.progress.emit(0, 0, "正在检查设备 ...")
        try:
            info = self._probe()
        except Exception as exc:
            self._fail(
                f"连不上设备 {self.host}:{self.port}：{exc}\n"
                "确认设备已开机、和电脑连的是同一 WiFi，并且固件里已经实现 OTA 服务"
                "（当前固件尚未实现，见 docs/ota_eval.md）。")
            return

        self.log.emit(
            f"设备在线：{info.get('version') or '未知版本'}，"
            f"空闲内存 {info.get('free_heap', '?')} 字节", "info")

        self._check_cancel()
        self.progress.emit(0, size, f"正在上传 {size / 1048576:.2f} MB 固件 ...")

        import http.client

        conn = http.client.HTTPConnection(self.host, self.port, timeout=120)
        sent = 0
        try:
            conn.putrequest("POST", "/ota")
            conn.putheader("Content-Type", "application/octet-stream")
            conn.putheader("Content-Length", str(size))
            if self.token:
                conn.putheader("X-OTA-Token", self.token)
            conn.endheaders()

            with open(self.firmware, "rb") as fh:
                while True:
                    self._check_cancel()
                    chunk = fh.read(self.CHUNK)
                    if not chunk:
                        break
                    conn.send(chunk)
                    sent += len(chunk)
                    self.progress.emit(sent, size, f"已发送 {sent * 100 // size}%")

            resp = conn.getresponse()
            body = resp.read().decode("utf-8", "replace")
            if resp.status not in (200, 204):
                raise RuntimeError(f"设备返回 HTTP {resp.status}：{body[:200]}")
            self.log.emit(f"设备已接收，正在校验并重启：{body[:120]}", "ok")
            self.done.emit({"error": None, "bytes": sent, "info": info})
        except WorkerCancelled as exc:
            self._fail(str(exc))
        except Exception as exc:
            self._fail(f"OTA 推送失败：{exc}")
        finally:
            conn.close()
