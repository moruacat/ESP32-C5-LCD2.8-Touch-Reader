#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 GB2312 → Unicode 映射表

srv_reader 打开中文 TXT 时要判断编码：是 UTF-8 就直接用，
是 GBK（老资源和网络小说绝大多数是这个）就得转成 UTF-8 才能交给 LVGL 显示。

ESP-IDF 没带 iconv，所以自带一张表。GB2312 覆盖 6763 个常用汉字和全套
标点符号，读正常中文文本够用；超出范围的字（生僻字、繁体扩展）会显示成占位符。

表按 GBK 编码升序排列，C 侧二分查找即可，单次查找约 13 次比较。

用法：
    python tools/gen_gbk_table.py

输出：
    main/assets/gbk_table.c
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "main", "assets", "gbk_table.c")

PER_LINE = 8


def collect():
    """枚举 GB2312 的区位码（区 0xA1~0xF7，位 0xA1~0xFE）"""
    pairs = []
    skipped = 0
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode("gb2312")
            except UnicodeDecodeError:
                skipped += 1
                continue
            cp = ord(ch)
            if cp > 0xFFFF:
                continue
            pairs.append((hi << 8 | lo, cp))
    return pairs, skipped


def main():
    pairs, skipped = collect()
    if not pairs:
        print("没有收集到任何字符，检查 Python 是否带 gb2312 编解码器")
        return 1

    out = []
    out.append("/* 由 tools/gen_gbk_table.py 自动生成，请勿手工修改 */")
    out.append("/* GB2312 → Unicode 映射表，共 %d 项，按 GBK 编码升序排列 */" % len(pairs))
    out.append('#include "gbk_table.h"')
    out.append("")
    out.append("const gbk_pair_t g_gbk_table[] = {")
    for i in range(0, len(pairs), PER_LINE):
        chunk = pairs[i:i + PER_LINE]
        out.append("    " + " ".join("{0x%04X, 0x%04X}," % p for p in chunk))
    out.append("};")
    out.append("")
    out.append("const int g_gbk_table_size = %d;" % len(pairs))
    out.append("")

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))

    print("已生成 %s" % os.path.normpath(OUT))
    print("  映射项 %d（跳过 %d 个空位），Flash 占用约 %d KB"
          % (len(pairs), skipped, len(pairs) * 4 // 1024))
    return 0


if __name__ == "__main__":
    sys.exit(main())
