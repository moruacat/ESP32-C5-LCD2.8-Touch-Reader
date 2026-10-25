#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
photo2raw —— 把图片批量转成固件能直接显示的 320x240 RGB565 裸数据(.raw)

用法:
    python photo2raw.py 图片目录 -o E:\\photos          # 目录批量（递归子目录）
    python photo2raw.py a.png b.webp -o E:\\photos      # 指定若干文件
    python photo2raw.py 图片目录 --overwrite           # 覆盖已存在的同名输出
    python photo2raw.py 图片目录 --auto-rotate         # 竖图自动逆时针转 90°

输出:
    <源文件名>.raw      纯像素数据，没有文件头
                        320x240 RGB565，小端（低字节在前），固定 153600 字节
                        直接拷进 SD 卡的 /photos 目录即可

转换规则:
    - 等比缩放到不超过 320x240（只缩小，绝不放大），再居中贴到 320x240 黑底画布
    - 不裁剪，尺寸不足的方向留黑边
    - 加 --auto-rotate 时，若把图片逆时针旋转 90° 能显示更多像素，就自动旋转；
      横图、方图和旋转后不会更大的图保持原样
    - 支持的输入：PNG / JPG / JPEG / WEBP（后缀大小写不敏感）

重名:
    默认不覆盖：同名输出已存在时自动加序号 name_1.raw、name_2.raw …
    加 --overwrite 则直接覆盖同名输出

依赖: Python 3.10+；Pillow（必须）；numpy（可选，装了打包更快，
      没装自动退回纯 Python，两者输出逐字节一致）
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

try:
    from PIL import Image
except ImportError:                          # 没装 Pillow 时先让它 import 过去，
    Image = None                             # 真正解码时再抛出可读的错误

try:
    import numpy as _np
except ImportError:                          # numpy 只是加速用，没有也能跑
    _np = None

# ===================== 与固件对齐的常量（改这里要同步改固件） =====================

TARGET_W = 320
TARGET_H = 240
RAW_BYTES = TARGET_W * TARGET_H * 2          # 153600，输出必须正好这么多字节

IMAGE_EXTS = {".png", ".jpg", ".jpeg", ".webp"}

# 单个文件的转换结果
STATUS_OK = "ok"
STATUS_SKIP = "skip"
STATUS_FAIL = "fail"


class ConvertError(Exception):
    """转换失败，消息直接给用户看"""


# ===================== 自动旋转 =====================

def _fit_size(w: int, h: int) -> tuple[float, float]:
    """等比缩放到 320x240 画布后的实际尺寸（只缩小，不放大）。"""
    scale = min(TARGET_W / w, TARGET_H / h, 1.0)
    return w * scale, h * scale


def should_rotate_ccw(width: int, height: int) -> bool:
    """逆时针旋转 90° 后能显示的像素是否更多。

    比较的是「等比缩放 + 居中」后的实际面积；横图、方图以及旋转后
    反而更小的图都返回 False。加 0.5 是为了吃掉浮点误差。
    """
    if width <= 0 or height <= 0:
        return False
    w0, h0 = _fit_size(width, height)
    w1, h1 = _fit_size(height, width)
    return w1 * h1 > w0 * h0 + 0.5


def orient_for_display(im, auto_rotate: bool):
    """按需逆时针旋转 90°，返回 (图像, 是否旋转过)。"""
    if not auto_rotate or not should_rotate_ccw(*im.size):
        return im, False
    return im.transpose(Image.Transpose.ROTATE_90), True


# ===================== RGB565 打包 =====================

def _pack_rgb565_py(px: bytes, count: int) -> bytes:
    """纯 Python 打包：px 是 RGBRGB… 连续字节，count 是像素数

    公式与 tools/epub2ebf/epub2ebf.py 的 convert_cover() 完全一致：
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    低字节在前（LVGL RGB565 的内存布局），不依赖运行机器的字节序。
    """
    out = bytearray(count * 2)
    for i in range(count):
        r = px[3 * i]
        g = px[3 * i + 1]
        b = px[3 * i + 2]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[2 * i] = v & 0xFF
        out[2 * i + 1] = v >> 8
    return bytes(out)


def _pack_rgb565_np(px: bytes, w: int, h: int) -> bytes:
    """numpy 加速打包，逐字节等价于 _pack_rgb565_py"""
    arr = _np.frombuffer(px, dtype=_np.uint8).reshape(h, w, 3)
    r = arr[:, :, 0].astype(_np.uint16)
    g = arr[:, :, 1].astype(_np.uint16)
    b = arr[:, :, 2].astype(_np.uint16)
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    out = _np.empty((h, w, 2), dtype=_np.uint8)
    out[:, :, 0] = (v & 0xFF).astype(_np.uint8)     # 低字节在前
    out[:, :, 1] = (v >> 8).astype(_np.uint8)
    return out.tobytes()


def pack_rgb565(px: bytes) -> bytes:
    """RGB 连续字节（TARGET_W x TARGET_H）→ RGB565 小端字节流"""
    if _np is not None:
        return _pack_rgb565_np(px, TARGET_W, TARGET_H)
    return _pack_rgb565_py(px, TARGET_W * TARGET_H)


def image_to_raw(im) -> bytes:
    """PIL 图像 → 320x240 RGB565 小端字节流（等比缩小 + 居中黑底）"""
    im = im.convert("RGB")
    im.thumbnail((TARGET_W, TARGET_H))       # 等比缩到框内，小图不放大
    canvas = Image.new("RGB", (TARGET_W, TARGET_H), (0, 0, 0))
    canvas.paste(im, ((TARGET_W - im.width) // 2, (TARGET_H - im.height) // 2))
    return pack_rgb565(canvas.tobytes())


# ===================== 单文件转换 =====================

@dataclass
class Options:
    """一次转换的全部参数，CLI 和 GUI 都构造它"""
    out_root: Path | None = None            # None = 与源文件同目录
    overwrite: bool = False                 # 同名输出已存在时直接覆盖
    auto_rotate: bool = False               # 竖图自动逆时针旋转 90° 后显示面积更大
    quiet: bool = False
    log: Callable[[str], None] | None = None        # 正常信息
    warn: Callable[[str], None] | None = None       # 警告/错误

    def info(self, msg: str) -> None:
        if self.log is not None and not self.quiet:
            self.log(msg)

    def warning(self, msg: str) -> None:
        if self.warn is not None:
            self.warn(msg)
        elif not self.quiet:
            print(msg, file=sys.stderr)


def load_image(src: Path):
    """读文件并解码成 PIL 图像"""
    if Image is None:
        raise ConvertError("没装 Pillow，无法解码图片（pip install Pillow）")
    try:
        im = Image.open(src)
        im.load()
    except Exception as e:
        raise ConvertError(f"图片打不开（{type(e).__name__}: {e}）")
    return im


def resolve_target(src: Path, out_dir: Path, overwrite: bool) -> Path:
    """决定输出路径：不覆盖时，同名就自动加序号 name_1.raw、name_2.raw …"""
    target = out_dir / (src.stem + ".raw")
    if overwrite or not target.exists():
        return target
    n = 1
    while True:
        cand = out_dir / f"{src.stem}_{n}.raw"
        if not cand.exists():
            return cand
        n += 1


def convert_one(src: Path, opt: Options) -> str:
    """转一个文件，返回 STATUS_OK / STATUS_FAIL"""
    try:
        im = load_image(src)
        w, h = im.size
        im, rotated = orient_for_display(im, opt.auto_rotate)
        data = image_to_raw(im)
        if len(data) != RAW_BYTES:
            raise ConvertError(f"内部错误：生成 {len(data)} 字节，应为 {RAW_BYTES}")

        out_dir = opt.out_root if opt.out_root is not None else src.parent
        out_dir.mkdir(parents=True, exist_ok=True)
        target = resolve_target(src, out_dir, opt.overwrite)
        target.write_bytes(data)

        turned = "  逆时针旋转 90°" if rotated else ""
        opt.info(f"  {w}x{h} → {TARGET_W}x{TARGET_H}{turned}  输出 {target.name}"
                 f"（{len(data)} 字节）")
        return STATUS_OK
    except ConvertError as e:
        opt.warning(f"[失败] {src.name}: {e}")
        return STATUS_FAIL
    except Exception as e:                   # 单个文件出错不该中断批量转换
        opt.warning(f"[失败] {src.name}: {type(e).__name__}: {e}")
        return STATUS_FAIL


def collect_inputs(paths: list[str], warn: Callable[[str], None] | None = None
                   ) -> tuple[list[Path], int]:
    """把 CLI/GUI 给的路径摊平成图片文件列表（目录递归、按绝对路径去重）

    返回 (文件列表, 被跳过的输入数)。找不到的路径、非图片后缀、空目录、
    重复出现的文件都算"跳过"，并在 warn 里说明原因。
    """
    def _warn(msg: str) -> None:
        if warn is not None:
            warn(msg)

    files: list[Path] = []
    seen: set[Path] = set()
    skipped = 0

    def _add(path: Path) -> None:
        nonlocal skipped
        try:
            key = path.resolve()
        except OSError:
            key = path.absolute()
        if key in seen:
            skipped += 1
            _warn(f"[跳过] 重复的输入 {path}")
            return
        seen.add(key)
        files.append(path)

    for p in paths:
        path = Path(p)
        if path.is_dir():
            found = sorted(x for x in path.rglob("*")
                           if x.is_file() and x.suffix.lower() in IMAGE_EXTS)
            if not found:
                skipped += 1
                _warn(f"[跳过] 目录里没有图片 {path}")
                continue
            for x in found:
                _add(x)
        elif path.is_file():
            if path.suffix.lower() in IMAGE_EXTS:
                _add(path)
            else:
                skipped += 1
                _warn(f"[跳过] 不是支持的图片格式 {path.name}")
        else:
            skipped += 1
            _warn(f"[跳过] 找不到 {p}")

    return files, skipped


# ===================== 主流程 =====================

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="photo2raw",
        description="把图片批量转成 320x240 RGB565 小端裸数据(.raw)，用于 SD 卡 /photos",
        epilog="输出默认放在源文件同目录；输出没有文件头，直接拷进 SD 卡即可。",
    )
    ap.add_argument("input", nargs="+", help="图片文件或包含图片的目录（可多个，目录递归）")
    ap.add_argument("-o", "--out", help="输出目录（默认与源文件同目录）")
    ap.add_argument("--overwrite", action="store_true", help="覆盖已存在的同名 .raw")
    ap.add_argument("--auto-rotate", action="store_true",
                    help="竖图自动逆时针旋转 90°，让画面尽量占满 320x240")
    ap.add_argument("-q", "--quiet", action="store_true", help="只报错误")
    args = ap.parse_args(argv)

    if Image is None:
        print("没装 Pillow，无法解码图片（pip install Pillow）", file=sys.stderr)
        return 1

    opt = Options(
        out_root=Path(args.out) if args.out else None,
        overwrite=args.overwrite,
        auto_rotate=args.auto_rotate,
        quiet=args.quiet,
        log=print,
    )

    files, skipped = collect_inputs(args.input, opt.warning)
    if not files:
        print("没有可转换的图片", file=sys.stderr)
        return 1

    if opt.out_root is not None:
        opt.out_root.mkdir(parents=True, exist_ok=True)

    ok = fail = 0
    for f in files:
        opt.info(f"[转换] {f.name}")
        st = convert_one(f, opt)
        if st == STATUS_OK:
            ok += 1
        elif st == STATUS_FAIL:
            fail += 1

    opt.info(f"完成：成功 {ok}，跳过 {skipped}，失败 {fail}"
             + ("（失败原因见上面的 [失败] 行）" if fail else ""))
    return 0 if fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
