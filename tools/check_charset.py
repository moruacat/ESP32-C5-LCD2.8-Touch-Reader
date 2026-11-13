#!/usr/bin/env python3
"""扫描 EBF 电子书，找出字体字符集里没有的字符（会显示成方块的那些）。

用法：
    python tools/check_charset.py "D:\\path\\to\\book.eb"
    python tools/check_charset.py "D:\\path\\to\\book.eb\\chapter.1.正文.ebl"

字符集取 main/assets/fonts/charset.hex —— 那是 gen_font.py 最终交给
lv_font_conv 的码点表，也就是 font_ui_14 里真实存在的字形。

EBF 章节是 UTF-8 标记文本：<title>..</> + <body> + 正文，
只扫 <body> 之后的内容，避免把标记本身算进去。

退出码：0 = 全部在字符集内；2 = 有缺字；1 = 用法/路径错误。
"""

import os
import sys
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHARSET = os.path.join(ROOT, "main", "assets", "fonts", "charset.hex")
BODY_TAG = "<body>"


def load_charset(path):
    cps = set()
    with open(path, encoding="ascii") as f:
        for line in f:
            line = line.strip()
            if line:
                cps.add(int(line, 16))
    return cps


def read_body(path):
    """读 .ebl 并截掉 <body> 之前的元数据。"""
    with open(path, encoding="utf-8", errors="replace") as f:
        raw = f.read()
    i = raw.find(BODY_TAG)
    return raw[i + len(BODY_TAG):] if i >= 0 else raw


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    target = sys.argv[1]
    if os.path.isfile(target):
        files = [target]
    elif os.path.isdir(target):
        files = sorted(
            os.path.join(target, n) for n in os.listdir(target) if n.endswith(".ebl")
        )
    else:
        print("路径不存在: %s" % target)
        return 1

    if not files:
        print("没有找到 .ebl 文件: %s" % target)
        return 1

    charset = load_charset(CHARSET)
    print("字符集: %s（%d 个码点）" % (os.path.relpath(CHARSET, ROOT), len(charset)))
    print("待扫描: %d 个文件" % len(files))

    hits = Counter()        # 字符 -> 出现次数
    first_ctx = {}          # 字符 -> (文件名, 上下文)
    total_chars = 0
    bad_files = 0

    for path in files:
        try:
            text = read_body(path)
        except OSError:
            bad_files += 1
            continue
        total_chars += len(text)
        for idx, ch in enumerate(text):
            cp = ord(ch)
            if cp < 0x20:           # 换行/缩进等控制字符不算缺字
                continue
            if cp in charset:
                continue
            hits[ch] += 1
            if ch not in first_ctx:
                s = max(0, idx - 12)
                e = min(len(text), idx + 13)
                ctx = text[s:e].replace("\r", " ").replace("\n", " ")
                first_ctx[ch] = (os.path.basename(path), ctx)

    print("合计 %d 个字符，%d 个文件读取失败" % (total_chars, bad_files))

    if not hits:
        print("\n结论：全部字符都在字符集内。")
        return 0

    print("\n结论：%d 个字符不在字符集内（按出现次数排序）：" % len(hits))
    print("%-4s %-8s %-6s  %s" % ("字符", "码点", "次数", "首次出现"))
    for ch, n in hits.most_common():
        fname, ctx = first_ctx[ch]
        print("%-4s U+%04X  %-6d  %s | %s" % (ch, ord(ch), n, fname, ctx))
    return 2


if __name__ == "__main__":
    sys.exit(main())
