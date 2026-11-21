#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成阅读器「多档字号」用的二进制字库，并打包成 font 分区镜像。

项目里 14px 的 font_ui_14 编在固件里；16/18/20/22px 全字库太大，用
lv_font_conv 生成 LVGL 的 .bin，运行时由 lv_binfont_create_from_buffer()
加载。字库同时放两个地方：

  1. `flash_assets/fonts/` → 打包进 Flash 的 `font` SPIFFS 分区（无 SD 也能用）
  2. 可选复制到 SD 卡 `/fonts`（优先级更高，方便换字库不重刷）

产物：
  font/font_ui_16.bin / 18 / 20 / 22
  flash_assets/fonts/（同上，供 spiffsgen 打包）
  build/font.bin（font 分区镜像，CI 面板烧录时自动带上）

用法：
    python tools/gen_read_fonts.py                 # 生成缺失档位 + 打包镜像
    python tools/gen_read_fonts.py 18 22           # 只补这两档
    python tools/gen_read_fonts.py --force         # 全部重新生成
    python tools/gen_read_fonts.py --to-sim        # 顺便拷到 sim/sd/fonts
    python tools/gen_read_fonts.py --no-image      # 不生成 build/font.bin
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
ASSET_DIR = os.path.join(ROOT, "flash_assets", "fonts")
DEFAULT_IMAGE = os.path.join(ROOT, "build", "font.bin")
SIZES = (16, 18, 20, 22)
BPP = 4
DEFAULT_PARTITION_SIZE = 0xB00000   # 与 partitions.csv 的 font 分区一致

HANZI_TTF = os.path.join(ROOT, "font", "HarmonyOS_Sans_SC_Medium.ttf")

IDF_CANDIDATES = (
    r"D:\Dev-Kit\ESP-IDF\v6.1\esp-idf",
    r"C:\Espressif\frameworks\esp-idf-v6.1",
    r"C:\Espressif\esp-idf",
)


def find_spiffsgen():
    env = os.environ.get("IDF_PATH")
    for root in ([env] if env else []) + list(IDF_CANDIDATES):
        if not root:
            continue
        path = os.path.join(root, "components", "spiffs", "spiffsgen.py")
        if os.path.exists(path):
            return path
    return None


def build(size, hex_path, force):
    out = os.path.join(OUT_DIR, "font_ui_%d.bin" % size)
    if os.path.exists(out) and not force and os.path.getsize(out) > 1024:
        print("  %dpx 已存在，跳过（--force 可重生成）" % size)
        return True

    lvc = gen_font.find_lvc()
    if lvc is None:
        print("找不到 lv_font_conv（装 Node.js + npm i -g lv_font_conv，"
              "或用 LV_FONT_CONV 指向 lv_font_conv.js）")
        return False

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


def stage_assets():
    """把四档字库复制到 flash_assets/fonts/，供 spiffsgen 打包"""
    os.makedirs(ASSET_DIR, exist_ok=True)
    total = 0
    for size in SIZES:
        src = os.path.join(OUT_DIR, "font_ui_%d.bin" % size)
        if not os.path.exists(src):
            print("缺少 %s，镜像会不完整" % os.path.relpath(src, ROOT))
            return False
        shutil.copy2(src, ASSET_DIR)
        total += os.path.getsize(src)
    print("flash_assets/fonts/：%d 个文件，%.2f MB" % (len(SIZES), total / 1048576.0))
    return True


def make_image(out_image, partition_size):
    spiffsgen = find_spiffsgen()
    if spiffsgen is None:
        print("找不到 IDF 的 spiffsgen.py，跳过 font.bin（SD 卡字体仍可用）")
        print("可手动执行：python <IDF>/components/spiffs/spiffsgen.py %d %s %s"
              % (partition_size, ASSET_DIR, out_image))
        return False

    os.makedirs(os.path.dirname(os.path.abspath(out_image)), exist_ok=True)
    print("打包 SPIFFS 镜像：%d 字节分区 -> %s" % (partition_size, out_image))
    ret = subprocess.run([sys.executable, spiffsgen, str(partition_size),
                          ASSET_DIR, out_image])
    if ret.returncode != 0:
        print("spiffsgen.py 失败（退出码 %d）" % ret.returncode)
        return False
    print("  -> %s（%.2f MB）" % (out_image, os.path.getsize(out_image) / 1048576.0))
    return True


def main():
    args = sys.argv[1:]
    to_sim = "--to-sim" in args
    no_image = "--no-image" in args
    force = "--force" in args
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
        ok = build(size, hex_path, force) and ok

    if not ok:
        return 1

    if not stage_assets():
        return 1

    if not no_image:
        make_image(DEFAULT_IMAGE, DEFAULT_PARTITION_SIZE)

    if to_sim:
        dst_dir = os.path.join(ROOT, "sim", "sd", "fonts")
        os.makedirs(dst_dir, exist_ok=True)
        for size in SIZES:
            src = os.path.join(OUT_DIR, "font_ui_%d.bin" % size)
            if os.path.exists(src):
                shutil.copy2(src, dst_dir)
        print("已拷到 sim/sd/fonts")

    print("完成。CI 面板烧录时会自动带上 build/font.bin（前提是先生成本文件）。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
