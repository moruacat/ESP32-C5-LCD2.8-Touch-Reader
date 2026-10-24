#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 LVGL 静态字体

用 LVGL 官方的 lv_font_conv（Node 工具）生成，它直接输出 lv_font_fmt_txt
的二进制格式，不用自己拼位图、算 cmap——那部分太容易出错。

字符集取 font/hanzi.txt（常用汉字）和 main/assets/symbols.txt（界面上要用到的
符号）的并集，去重后排序。两个都读是为了防止某一边漏字：hanzi.txt 是汉字表，
symbols.txt 里还有图标字符、上下标、日文假名这些。
字体本身没有的字（比如 TTF 里缺的生僻符号）lv_font_conv 会自动跳过并警告。

音标（IPA）不在这套中文 TTF 里，所以单独烘一个 font_ipa_14：字符集是 ASCII
可打印字符 + IPA 符号，字体用系统自带的 Arial（覆盖全部 IPA 字符）。音标文本
里混有普通拉丁字母，所以 ASCII 也要一起带上。

用法：
    python tools/gen_font.py              # 重新生成全部目标字体
    python tools/gen_font.py ui_14        # 只生成指定的那个

前置：Node.js + lv_font_conv。
    npx --no-install lv_font_conv --version   # 能打出 1.5.x 就行
"""

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# 中文/UI 字体：鸿蒙（项目自带）
HANZI_TTF = os.path.join(ROOT, "font", "HarmonyOS_Sans_SC_Medium.ttf")
# 音标字体：系统 Arial。这一项只为了拿到 IPA 字形，换个覆盖 IPA 的字体也行
IPA_TTF = r"C:\Windows\Fonts\arial.ttf"

CHARSET_FILES = [
    os.path.join(ROOT, "font", "hanzi.txt"),
    os.path.join(ROOT, "main", "assets", "symbols.txt"),
]
OUT_DIR = os.path.join(ROOT, "main", "assets", "fonts")

# 英语词典里常见的 IPA 符号（含重音、长短音、元音、辅音）
IPA_EXTRA = "ˈˌːˑ͜͡əɚɜɝɐɪʊʏɛœʌɔɒɑæɨʉʃʒʧʤθðçɡŋɲɹɾʔʰʲʷⁿ"

# 目标字体：key -> (输出名, 字号, bpp, 字体文件, 字符集种类)
# 时钟那个 48px 字体只有数字和冒号，字符集固定，不用重新生成
TARGETS = {
    "ui_14":  ("font_ui_14",  14, 4, HANZI_TTF, "ui"),
    "ipa_14": ("font_ipa_14", 14, 4, IPA_TTF,   "ipa"),
}


def collect_chars(kind):
    """按种类收集字符集

    用 set 而不是 dict.fromkeys 保序：最后要按码点排序，顺序没意义。
    """
    if kind == "ipa":
        chars = {chr(c) for c in range(0x20, 0x7F)}   # ASCII 可打印
        chars.update(IPA_EXTRA)                       # 音标本身的符号
        print("  音标字符集：ASCII + %d 个 IPA 符号" % len(IPA_EXTRA))
        return sorted(chars)

    chars = set()
    for path in CHARSET_FILES:
        if not os.path.exists(path):
            print("  跳过（不存在）: %s" % path)
            continue
        with open(path, encoding="utf-8") as f:
            chars.update(f.read())
        print("  读入 %s" % os.path.relpath(path, ROOT))

    for c in ("\n", "\r", "\t"):
        chars.discard(c)
    return sorted(chars)


def write_hex(chars, kind):
    """写成 lv_font_conv 认的 @file 格式：每行一个十六进制码点"""
    hex_path = os.path.join(OUT_DIR, "charset.hex" if kind == "ui"
                            else "charset_%s.hex" % kind)
    with open(hex_path, "w", encoding="ascii", newline="\n") as f:
        for ch in chars:
            f.write("%x\n" % ord(ch))
    return hex_path


def build(name, size, bpp, ttf, hex_path):
    npx = shutil.which("npx") or shutil.which("npx.cmd")
    if npx is None:
        print("找不到 npx，请先装 Node.js")
        return False

    out_c = os.path.join(OUT_DIR, name + ".c")
    cmd = [
        npx, "--no-install", "lv_font_conv",
        "--font", ttf,
        "--format", "lvgl",
        "--size", str(size),
        "--bpp", str(bpp),
        "--no-compress",        # 压缩版要额外解压开销，屏小、字少，不划算
        "--no-prefilter",
        "--lv-include", "lvgl.h",
        "--lv-font-name", name,
        "--symbols", "@" + hex_path,
        "-o", out_c,
    ]
    print("  生成 %s ..." % name)
    ret = subprocess.run(cmd, cwd=ROOT)
    if ret.returncode != 0:
        print("  lv_font_conv 失败，返回码 %d" % ret.returncode)
        return False

    kb = os.path.getsize(out_c) / 1024.0
    print("  -> %s（%.0f KB 源码）" % (os.path.relpath(out_c, ROOT), kb))
    return True


def main():
    want = set(sys.argv[1:]) or set(TARGETS.keys())

    print("生成字体：")
    hex_cache = {}
    ok = True
    for key in sorted(want):
        if key not in TARGETS:
            print("  未知目标: %s（可选：%s）" % (key, ", ".join(sorted(TARGETS))))
            ok = False
            continue

        name, size, bpp, ttf, kind = TARGETS[key]
        if not os.path.exists(ttf):
            print("  找不到字体文件: %s（%s 跳过）" % (ttf, key))
            ok = False
            continue

        if kind not in hex_cache:
            print("  收集字符集（%s）：" % kind)
            hex_cache[kind] = write_hex(collect_chars(kind), kind)

        if not build(name, size, bpp, ttf, hex_cache[kind]):
            ok = False

    return 0 if ok else 1



if __name__ == "__main__":
    sys.exit(main())
