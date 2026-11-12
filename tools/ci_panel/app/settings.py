# -*- coding: utf-8 -*-
"""本机设置与 GitHub Token 的安全保存。

Token 使用 Windows DPAPI 按当前用户加密后再写入 QSettings，配置文件里
不会出现明文。DPAPI 解密只能在同一台 Windows 机器的同一用户下完成。
"""

from __future__ import annotations

import base64
import ctypes
import os
import sys
from ctypes import wintypes

from PySide6.QtCore import QSettings, QByteArray

APP_ORG = "MORUA"
APP_NAME = "CIFirmwareHelper"

_TOKEN_PREFIX = "dpapi:"
_ENTROPY = b"MORUA.CIFirmwareHelper.v1"


class _DataBlob(ctypes.Structure):
    _fields_ = [
        ("cbData", wintypes.DWORD),
        ("pbData", ctypes.POINTER(ctypes.c_char)),
    ]


def _blob(data: bytes):
    buffer = ctypes.create_string_buffer(data, len(data))
    blob = _DataBlob(
        len(data),
        ctypes.cast(buffer, ctypes.POINTER(ctypes.c_char)),
    )
    return blob, buffer


if sys.platform == "win32":
    _crypt32 = ctypes.WinDLL("crypt32", use_last_error=True)
    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

    _crypt32.CryptProtectData.argtypes = [
        ctypes.POINTER(_DataBlob), wintypes.LPCWSTR,
        ctypes.POINTER(_DataBlob), ctypes.c_void_p, ctypes.c_void_p,
        wintypes.DWORD, ctypes.POINTER(_DataBlob),
    ]
    _crypt32.CryptProtectData.restype = wintypes.BOOL
    _crypt32.CryptUnprotectData.argtypes = [
        ctypes.POINTER(_DataBlob), ctypes.POINTER(wintypes.LPWSTR),
        ctypes.POINTER(_DataBlob), ctypes.c_void_p, ctypes.c_void_p,
        wintypes.DWORD, ctypes.POINTER(_DataBlob),
    ]
    _crypt32.CryptUnprotectData.restype = wintypes.BOOL
    _kernel32.LocalFree.argtypes = [wintypes.HLOCAL]
    _kernel32.LocalFree.restype = wintypes.HLOCAL


def secret_storage_available() -> bool:
    return sys.platform == "win32"


def _raise_last_error(prefix: str) -> None:
    code = ctypes.get_last_error() if sys.platform == "win32" else 0
    raise OSError(code, f"{prefix}（Windows 错误 {code}）")


def protect_secret(plain: str) -> str:
    """把 Token 用当前 Windows 用户的 DPAPI 加密成可写入 QSettings 的文本。"""
    if not secret_storage_available():
        raise RuntimeError("当前系统不支持 Windows DPAPI，不能自动保存 Token")
    if not plain:
        return ""

    data = plain.encode("utf-8")
    in_blob, in_buf = _blob(data)
    entropy_blob, entropy_buf = _blob(_ENTROPY)
    out_blob = _DataBlob()

    ok = _crypt32.CryptProtectData(
        ctypes.byref(in_blob), "MORUA CI Firmware Helper",
        ctypes.byref(entropy_blob), None, None, 0x1,
        ctypes.byref(out_blob),
    )
    # 保活输入缓冲区；Python 的引用计数在函数返回前都不会释放。
    _ = (in_buf, entropy_buf)
    if not ok:
        _raise_last_error("加密 GitHub Token 失败")

    try:
        cipher = ctypes.string_at(out_blob.pbData, out_blob.cbData)
    finally:
        if out_blob.pbData:
            _kernel32.LocalFree(ctypes.cast(out_blob.pbData, wintypes.HLOCAL))

    return _TOKEN_PREFIX + base64.b64encode(cipher).decode("ascii")


def unprotect_secret(stored: str) -> str:
    """解开 protect_secret() 保存的文本；损坏或换用户时抛 OSError。"""
    if not stored:
        return ""
    if not stored.startswith(_TOKEN_PREFIX):
        raise ValueError("Token 保存格式不受支持")
    if not secret_storage_available():
        raise RuntimeError("当前系统不支持 Windows DPAPI")

    cipher = base64.b64decode(stored[len(_TOKEN_PREFIX):], validate=True)
    in_blob, in_buf = _blob(cipher)
    entropy_blob, entropy_buf = _blob(_ENTROPY)
    out_blob = _DataBlob()
    description = wintypes.LPWSTR()

    ok = _crypt32.CryptUnprotectData(
        ctypes.byref(in_blob), ctypes.byref(description),
        ctypes.byref(entropy_blob), None, None, 0x1,
        ctypes.byref(out_blob),
    )
    _ = (in_buf, entropy_buf)
    if description:
        _kernel32.LocalFree(ctypes.cast(description, wintypes.HLOCAL))
    if not ok:
        _raise_last_error("解密 GitHub Token 失败（可能更换了 Windows 用户）")

    try:
        data = ctypes.string_at(out_blob.pbData, out_blob.cbData)
    finally:
        if out_blob.pbData:
            _kernel32.LocalFree(ctypes.cast(out_blob.pbData, wintypes.HLOCAL))

    return data.decode("utf-8")


class Prefs:
    """窗口偏好的轻量封装。"""

    def __init__(self) -> None:
        self._s = QSettings(APP_ORG, APP_NAME)

    def text(self, key: str, default: str = "") -> str:
        value = self._s.value(key, default)
        return default if value is None else str(value)

    def boolean(self, key: str, default: bool = False) -> bool:
        value = self._s.value(key, default)
        if isinstance(value, bool):
            return value
        return str(value).strip().lower() in {"1", "true", "yes", "on"}

    def set(self, key: str, value) -> None:
        self._s.setValue(key, value)

    def remove(self, key: str) -> None:
        self._s.remove(key)

    def sync(self) -> None:
        self._s.sync()

    # ---------------- Token ----------------

    def load_token(self) -> tuple[str, str]:
        """返回 (token, 警告文本)。环境变量 GITHUB_TOKEN 作为兜底。"""
        remember = self.boolean("github/remember_token", False)
        warning = ""
        if remember:
            stored = self.text("github/token_dpapi")
            if stored:
                try:
                    return unprotect_secret(stored), ""
                except Exception as exc:
                    warning = f"读取已保存的 Token 失败：{exc}"
        env_token = os.environ.get("GITHUB_TOKEN", "").strip()
        return env_token, warning

    def save_token(self, token: str, remember: bool) -> str:
        """保存或清除 Token；返回需要展示给用户的警告，空字符串表示成功。"""
        self.set("github/remember_token", bool(remember))
        if not remember:
            self.remove("github/token_dpapi")
            self.sync()
            return ""
        if not token.strip():
            self.remove("github/token_dpapi")
            self.sync()
            return ""
        try:
            encrypted = protect_secret(token.strip())
        except Exception as exc:
            self.remove("github/token_dpapi")
            self.sync()
            return f"Token 未保存：{exc}"
        self.set("github/token_dpapi", encrypted)
        self.sync()
        return ""

    def geometry(self) -> QByteArray | None:
        value = self._s.value("window/geometry")
        if isinstance(value, QByteArray):
            return value
        if isinstance(value, (bytes, bytearray)):
            return QByteArray(value)
        return None

    def set_geometry(self, value: QByteArray) -> None:
        self._s.setValue("window/geometry", value)
        self._s.sync()
