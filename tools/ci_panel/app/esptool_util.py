# -*- coding: utf-8 -*-
"""查找可用的 esptool。

优先使用 ESP-IDF 安装目录里的 esptool.exe，避免用户必须把 IDF 环境
变量带进 GUI 进程。找不到时仍可下载固件，只是烧录按钮会提示安装。
"""

from __future__ import annotations

import importlib.util
import os
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True, slots=True)
class EsptoolTool:
    cmd: tuple[str, ...]
    label: str


def _glob_esptool_roots() -> list[Path]:
    roots: list[Path] = []
    idf_tools = os.environ.get("IDF_TOOLS_PATH", "").strip()
    if idf_tools:
        roots.append(Path(idf_tools))
    roots.append(Path(r"C:\Espressif"))
    home = os.environ.get("USERPROFILE", "").strip()
    if home:
        roots.append(Path(home) / ".espressif")

    unique: list[Path] = []
    seen: set[str] = set()
    for root in roots:
        key = str(root).lower()
        if key not in seen:
            seen.add(key)
            unique.append(root)
    return unique


def find_esptool() -> EsptoolTool | None:
    patterns = (
        ("tools", "python", "*", "venv", "Scripts", "esptool.exe"),
        ("python_env", "*", "Scripts", "esptool.exe"),
    )
    for root in _glob_esptool_roots():
        for parts in patterns:
            try:
                matches = sorted(
                    (p for p in root.glob(str(Path(*parts))) if p.is_file()),
                    key=lambda p: str(p).lower(),
                    reverse=True,
                )
            except OSError:
                continue
            for path in matches:
                return EsptoolTool((str(path),), str(path))

    exe = shutil.which("esptool") or shutil.which("esptool.exe")
    if exe:
        return EsptoolTool((exe,), exe)

    try:
        if importlib.util.find_spec("esptool") is not None:
            return EsptoolTool((sys.executable, "-u", "-m", "esptool"),
                               f"{sys.executable} -u -m esptool")
    except (ImportError, ValueError):
        pass
    return None
