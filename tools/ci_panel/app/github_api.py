# -*- coding: utf-8 -*-
"""GitHub Actions API 的只读封装。

只依赖 Python 标准库，不把网络请求散落在窗口代码里。所有公开接口都
返回中文错误信息，方便直接显示给用户。
"""

from __future__ import annotations

import json
import socket
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any

API_ROOT = "https://api.github.com"
USER_AGENT = "MORUA-CI-Firmware-Helper/0.1"


class GitHubError(RuntimeError):
    """可直接展示给用户的 GitHub API 错误。"""


class _StopRedirect(urllib.request.HTTPRedirectHandler):
    """不让 urllib 把 Authorization 带到 Azure 临时下载地址。"""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def _opener() -> urllib.request.OpenerDirector:
    return urllib.request.build_opener(_StopRedirect())


def _parse_datetime(value: str | None) -> datetime | None:
    if not value:
        return None
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None


def _format_local_time(value: str | None) -> str:
    dt = _parse_datetime(value)
    if dt is None:
        return value or ""
    try:
        dt = dt.astimezone()
    except ValueError:
        return value or ""
    now = datetime.now(dt.tzinfo)
    if dt.date() == now.date():
        return dt.strftime("今天 %H:%M")
    if dt.year == now.year:
        return dt.strftime("%m-%d %H:%M")
    return dt.strftime("%Y-%m-%d %H:%M")


def _format_duration(start: str | None, end: str | None) -> str:
    a = _parse_datetime(start)
    b = _parse_datetime(end)
    if a is None or b is None:
        return ""
    seconds = max(0, int((b - a).total_seconds()))
    if seconds < 60:
        return f"{seconds} 秒"
    minutes, seconds = divmod(seconds, 60)
    if minutes < 60:
        return f"{minutes} 分 {seconds} 秒"
    hours, minutes = divmod(minutes, 60)
    return f"{hours} 小时 {minutes} 分"


@dataclass(slots=True)
class WorkflowRun:
    id: int
    name: str
    display_title: str
    status: str
    conclusion: str | None
    head_sha: str
    head_branch: str
    event: str
    created_at: str
    updated_at: str
    run_started_at: str | None
    html_url: str
    run_number: int
    run_attempt: int

    @classmethod
    def from_api(cls, data: dict[str, Any]) -> "WorkflowRun":
        commit = data.get("head_commit") or {}
        title = data.get("display_title") or ""
        if not title:
            lines = (commit.get("message") or "").splitlines()
            if lines:
                title = lines[0]
        return cls(
            id=int(data.get("id") or 0),
            name=str(data.get("name") or ""),
            display_title=str(title or data.get("name") or "未命名运行"),
            status=str(data.get("status") or "unknown"),
            conclusion=data.get("conclusion"),
            head_sha=str(data.get("head_sha") or ""),
            head_branch=str(data.get("head_branch") or ""),
            event=str(data.get("event") or ""),
            created_at=str(data.get("created_at") or ""),
            updated_at=str(data.get("updated_at") or ""),
            run_started_at=data.get("run_started_at"),
            html_url=str(data.get("html_url") or ""),
            run_number=int(data.get("run_number") or 0),
            run_attempt=int(data.get("run_attempt") or 1),
        )

    @property
    def short_sha(self) -> str:
        return self.head_sha[:7] if self.head_sha else "—"

    @property
    def status_text(self) -> str:
        if self.status != "completed":
            return {
                "queued": "排队中",
                "in_progress": "进行中",
                "requested": "等待中",
                "waiting": "等待中",
                "pending": "等待中",
            }.get(self.status, "进行中")
        return {
            "success": "成功",
            "failure": "失败",
            "cancelled": "已取消",
            "timed_out": "已超时",
            "skipped": "已跳过",
            "neutral": "已结束",
            "action_required": "需处理",
            "stale": "已过期",
        }.get(self.conclusion or "", self.conclusion or "未知")

    @property
    def status_kind(self) -> str:
        if self.status != "completed":
            return "info" if self.status == "in_progress" else "muted"
        if self.conclusion == "success":
            return "ok"
        if self.conclusion in {"failure", "timed_out", "action_required"}:
            return "err"
        if self.conclusion in {"cancelled", "skipped", "stale"}:
            return "muted"
        return "warn"

    @property
    def time_text(self) -> str:
        return _format_local_time(self.created_at)

    @property
    def duration_text(self) -> str:
        return _format_duration(self.run_started_at or self.created_at,
                                self.updated_at)

    @property
    def event_text(self) -> str:
        return {
            "push": "push",
            "pull_request": "PR",
            "workflow_dispatch": "手动",
            "schedule": "定时",
        }.get(self.event, self.event or "—")


@dataclass(slots=True)
class Artifact:
    id: int
    name: str
    size_in_bytes: int
    expired: bool
    created_at: str


class GitHubClient:
    def __init__(self, repo: str, token: str = "", timeout: int = 30) -> None:
        self.repo = repo.strip()
        self.token = token.strip()
        self.timeout = timeout
        self._opener = _opener()

    # ---------------- 底层请求 ----------------

    @property
    def _authorized(self) -> bool:
        return bool(self.token)

    def _headers(self, accept: str = "application/vnd.github+json") -> dict[str, str]:
        headers = {
            "Accept": accept,
            "User-Agent": USER_AGENT,
            "X-GitHub-Api-Version": "2022-11-28",
        }
        if self.token:
            headers["Authorization"] = f"Bearer {self.token}"
        return headers

    def _api_error(self, exc: urllib.error.HTTPError) -> GitHubError:
        body = ""
        try:
            raw = exc.read(65536)
            body = raw.decode("utf-8", errors="replace")
        except Exception:
            pass
        message = ""
        try:
            payload = json.loads(body) if body else {}
            message = str(payload.get("message") or "")
        except Exception:
            pass

        if exc.code == 401:
            return GitHubError("GitHub Token 无效或已过期，请重新填写。")
        if exc.code == 403:
            remaining = exc.headers.get("X-RateLimit-Remaining", "")
            if remaining == "0":
                reset = exc.headers.get("X-RateLimit-Reset", "")
                hint = f"（额度重置时间戳：{reset}）" if reset else ""
                return GitHubError("GitHub API 访问额度已用完" + hint)
            return GitHubError("GitHub 拒绝访问（403）。请确认 Token 有 Actions: Read 权限。")
        if exc.code == 404:
            return GitHubError("找不到仓库、运行或 artifact；请检查仓库名和权限。")
        detail = f"：{message}" if message else ""
        return GitHubError(f"GitHub API 返回 HTTP {exc.code}{detail}")

    def _json(self, url: str) -> dict[str, Any]:
        req = urllib.request.Request(url, headers=self._headers())
        try:
            with self._opener.open(req, timeout=self.timeout) as resp:
                raw = resp.read()
        except urllib.error.HTTPError as exc:
            raise self._api_error(exc) from exc
        except (urllib.error.URLError, TimeoutError, socket.timeout) as exc:
            reason = getattr(exc, "reason", exc)
            raise GitHubError(f"无法连接 GitHub：{reason}") from exc

        try:
            payload = json.loads(raw.decode("utf-8"))
        except Exception as exc:
            raise GitHubError("GitHub 返回的数据格式无法解析。") from exc
        if not isinstance(payload, dict):
            raise GitHubError("GitHub 返回的数据格式不符合预期。")
        return payload

    def _resolve_redirect(self, api_url: str) -> str:
        """请求需要认证的 API，只取 302 的临时下载地址，不跟随重定向。"""
        req = urllib.request.Request(api_url, headers=self._headers())
        try:
            with self._opener.open(req, timeout=self.timeout) as resp:
                # 正常情况下 GitHub 返回 302；如果直接返回 200，当前 URL 也可用。
                return resp.geturl()
        except urllib.error.HTTPError as exc:
            if exc.code in (301, 302, 303, 307, 308):
                location = exc.headers.get("Location") or ""
                if location:
                    return location
                raise GitHubError("GitHub 没有返回 artifact 下载地址。") from exc
            raise self._api_error(exc) from exc
        except (urllib.error.URLError, TimeoutError, socket.timeout) as exc:
            reason = getattr(exc, "reason", exc)
            raise GitHubError(f"无法解析下载地址：{reason}") from exc

    # ---------------- 业务接口 ----------------

    def list_runs(self, branch: str = "main", per_page: int = 30) -> list[WorkflowRun]:
        query = urllib.parse.urlencode({
            "branch": branch.strip(),
            "per_page": max(1, min(100, per_page)),
        })
        url = f"{API_ROOT}/repos/{self.repo}/actions/runs?{query}"
        payload = self._json(url)
        rows = payload.get("workflow_runs") or []
        if not isinstance(rows, list):
            raise GitHubError("GitHub 返回的构建列表格式不正确。")
        return [WorkflowRun.from_api(row) for row in rows if isinstance(row, dict)]

    def find_firmware_artifact(self, run_id: int) -> Artifact:
        url = f"{API_ROOT}/repos/{self.repo}/actions/runs/{run_id}/artifacts"
        payload = self._json(url)
        rows = payload.get("artifacts") or []
        if not isinstance(rows, list):
            raise GitHubError("GitHub 返回的 artifact 列表格式不正确。")
        for row in rows:
            if isinstance(row, dict) and row.get("name") == "firmware":
                return Artifact(
                    id=int(row.get("id") or 0),
                    name=str(row.get("name") or "firmware"),
                    size_in_bytes=int(row.get("size_in_bytes") or 0),
                    expired=bool(row.get("expired")),
                    created_at=str(row.get("created_at") or ""),
                )
        raise GitHubError("这次运行没有名为 firmware 的 artifact，可能编译尚未成功。")

    def resolve_artifact_url(self, artifact_id: int) -> str:
        url = f"{API_ROOT}/repos/{self.repo}/actions/artifacts/{artifact_id}/zip"
        return self._resolve_redirect(url)

    def resolve_logs_url(self, run_id: int) -> str:
        url = f"{API_ROOT}/repos/{self.repo}/actions/runs/{run_id}/logs"
        return self._resolve_redirect(url)
