# -*- coding: utf-8 -*-
"""局域网 mDNS 发现：找同一网段里开了 OTA 服务的 ClockReader 设备。

设备端约定（见 docs/ota_eval.md）：
    服务类型  _clockreader-ota._tcp.local.
    主机名    clockreader.local
    端口      8080（HTTP）
    TXT       可选，version=<固件版本>

找到设备后由 workers.OtaPushWorker 走 HTTP 推送固件。
没装 zeroconf 时返回明确的安装提示，窗口据此告诉用户，而不是静默失败。
"""

from __future__ import annotations

import time
from dataclasses import dataclass

SERVICE_TYPE = "_clockreader-ota._tcp.local."
SCAN_SECONDS = 3.0


@dataclass(slots=True)
class OtaDevice:
    """一台可 OTA 的设备。"""

    name: str
    host: str
    port: int
    version: str = ""

    @property
    def url(self) -> str:
        return f"http://{self.host}:{self.port}"

    def label(self) -> str:
        tail = f"  v{self.version}" if self.version else ""
        return f"{self.name}{tail} — {self.host}:{self.port}"


def _decode_txt(properties) -> dict[str, str]:
    result: dict[str, str] = {}
    for key, value in (properties or {}).items():
        try:
            k = key.decode("utf-8", "replace") if isinstance(key, bytes) else str(key)
            if isinstance(value, bytes):
                v = value.decode("utf-8", "replace")
            else:
                v = "" if value is None else str(value)
            result[k] = v
        except Exception:
            continue
    return result


def scan(seconds: float = SCAN_SECONDS) -> tuple[list[OtaDevice], str | None]:
    """扫描局域网内的 OTA 设备。

    返回 ``(设备列表, 错误说明)``：

    * 正常但没有设备            → ``([], None)``
    * zeroconf 未安装 / 扫描失败 → ``([], "原因")``
    """
    try:
        from zeroconf import ServiceBrowser, Zeroconf  # type: ignore
    except ImportError:
        return [], "未安装 zeroconf，请执行：python -m pip install zeroconf"

    found: dict[str, OtaDevice] = {}

    class _Listener:
        def add_service(self, zc, type_, name):
            info = zc.get_service_info(type_, name, timeout=1500)
            if info is None:
                return
            try:
                addresses = info.parsed_scoped_addresses()
            except Exception:
                addresses = []
            if not addresses:
                return
            txt = _decode_txt(info.properties)
            found[name] = OtaDevice(
                name=info.get_name() or name,
                host=addresses[0],
                port=int(info.port or 0),
                version=txt.get("version", ""),
            )

        def update_service(self, zc, type_, name):
            self.add_service(zc, type_, name)

        def remove_service(self, zc, type_, name):
            pass

    zc = None
    browser = None
    try:
        zc = Zeroconf()
        browser = ServiceBrowser(zc, SERVICE_TYPE, _Listener())
        time.sleep(seconds)          # 阻塞收集；调用方在 worker 线程里
    except Exception as exc:         # 网卡异常 / 权限问题等
        return [], f"mDNS 扫描失败：{exc}"
    finally:
        try:
            if browser is not None:
                browser.cancel()
        except Exception:
            pass
        try:
            if zc is not None:
                zc.close()
        except Exception:
            pass

    return sorted(found.values(), key=lambda d: d.name), None
