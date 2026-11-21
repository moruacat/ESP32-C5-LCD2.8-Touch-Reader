#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成阅读器「多档字号」用的二进制字库（font/font_ui_16.bin、font_ui_20.bin）。

项目里 14px 的 font_ui_14 是编进固件的 C 数组；16/20px 全字库太大
（1.7MB / 2.6MB），所以用 lv_font_conv 生成 LVGL 的 .bin 格式，运行时由
lv_binfont_create_from_buffer() 从 SD 卡 /fonts 加载。

用法：
    python tools/gen_read_fonts.py            # 生成 16 与 20
    python tools/gen_read_fonts.py 16         # 只生成 16
    python tools/gen_read_fonts.py --to-sim   # 生成后顺便拷到 sim/sd/fonts

生成完把 font/font_ui_*.bin 拷到 SD 卡根目录的 fonts/ 下，设备上
「设置 → 阅读 → 字号」就能循环到新档位。没拷文件时只有 14px 可选。
"""

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import gen_font  # noqa: E402  复用字符集收集与 lv_font_conv 定位

OUT_DIR = os.path.join(ROOT, "font")
SIZES = (16, 20)
BPP = 4

HANZI_TTF = os.path.join(ROOT, "font", "HarmonyOS_Sans_SC_Medium.ttf")


def build(size, hex_path):
    lvc = gen_font.find_lvc()
    if lvc is None:
        print("找不到 lv_font_conv（装 Node.js + npm i -g lv_font_conv，"
              "或用 LV_FONT_CONV 指向 lv_font_conv.js）")
        return False

    out = os.path.join(OUT_DIR, "font_ui_%d.bin" % size)
    cmd = lvc + [
        "--font", HANZI_TTF,
        "--format", "bin",
        "--size", str(size),
        "--bpp", str(BPP),
        "--no-compress",
        "--no-prefilter",
        "--symbols", "@" + hex_path,
        "-o", out,
    ]
    print("生成 %dpx 字库 …" % size)
    ret = subprocess.run(cmd, cwd=ROOT)
    if ret.returncode != 0:
        print("lv_font_conv 失败：%d" % ret.returncode)
        return False

    print("  -> %s（%.2f MB）" % (os.path.relpath(out, ROOT),
                                 os.path.getsize(out) / 1048576.0))
    return True


def main():
    args = sys.argv[1:]
    to_sim = "--to-sim" in args
    wanted = [int(a) for a in args if a.isdigit()] or list(SIZES)

    if not os.path.exists(HANZI_TTF):
        print("找不到字体文件 %s" % HANZI_TTF)
        return 1

    os.makedirs(OUT_DIR, exist_ok=True)
    hex_path = os.path.join(ROOT, "main", "assets", "fonts", "charset.hex")
    if not os.path.exists(hex_path):
        print("生成字符集 …")
        hex_path = gen_font.write_hex(gen_font.collect_chars("ui"), "ui")

    ok = True
    for size in wanted:
        if size not in SIZES:
            print("不支持的字号：%d（可选 %s）" % (size, ", ".join(map(str, SIZES))))
            ok = False
            continue
        ok = build(size, hex_path) and ok

    if to_sim and ok:
        dst_dir = os.path.join(ROOT, "sim", "sd", "fonts")
        os.makedirs(dst_dir, exist_ok=True)
        for size in wanted:
            src = os.path.join(OUT_DIR, "font_ui_%d.bin" % size)
            if os.path.exists(src):
                shutil.copy2(src, dst_dir)
                print("  -> %s" % os.path.relpath(os.path.join(dst_dir, os.path.basename(src)), ROOT))

    print("完成。把 font/font_ui_*.bin 拷到 SD 卡的 /fonts 目录即可在设置里选字号。")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
