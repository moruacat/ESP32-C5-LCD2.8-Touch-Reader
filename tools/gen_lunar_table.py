#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成农历数据表（srv_lunar.c 里的 YEAR_INFOS / YEAR_DAYS）。

数据来自 lunardate 包（GPLv2 的 lunar 项目数据，1900–2099）。脚本只在
换数据源或扩展年份范围时需要跑；平时固件直接用生成好的 C 数组。

用法：
    python tools/gen_lunar_table.py
"""

import os
import sys

try:
    import lunardate
except ImportError:
    print("先安装：python -m pip install lunardate")
    sys.exit(1)

infos = list(lunardate.YEAR_INFOS)
days = list(lunardate.YEAR_DAYS)
assert len(infos) == len(days) == 200, (len(infos), len(days))

out = []
out.append("/* 自动生成：tools/gen_lunar_table.py（lunardate 0.3.0，1900–2099） */")
out.append("static const uint32_t s_year_infos[] = {")
for i in range(0, len(infos), 8):
    row = ", ".join("0x%05X" % v for v in infos[i:i + 8])
    out.append("    %s," % row)
out.append("};")
out.append("")
out.append("static const uint16_t s_year_days[] = {")
for i in range(0, len(days), 12):
    row = ", ".join("%4d" % v for v in days[i:i + 12])
    out.append("    %s," % row)
out.append("};")

here = os.path.dirname(os.path.abspath(__file__))
path = os.path.join(here, "..", "main", "services", "lunar_table.inc")
path = os.path.abspath(path)
with open(path, "w", encoding="utf-8") as f:
    f.write("\n".join(out) + "\n")
print("已写入 %s（%d 项）" % (os.path.relpath(path, os.path.dirname(here)), len(infos)))
