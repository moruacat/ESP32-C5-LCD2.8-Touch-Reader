#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 ECDICT 生成词形变化表：`dict/variants.dat` + `dict/variants.idx`。

设备端「单词变形查询」用：输入 ran / better / studies，先经 dict.dat 的 lemma
还原到原形，再用原形在本表里取全部变形（过去式/过去分词/现在分词/三单/复数/
比较级/最高级）。

数据源：words/ecdict.csv 的 exchange 字段
    p=过去式  d=过去分词  i=现在分词  3=三单  s=复数  r=比较级  t=最高级
    0=原形（带 0 的行是变形词，跳过，它的形式由原形行提供）

输出格式（每行）：
    base<TAB>p:ran|d:run|i:running|3:runs

索引文件与 dict.idx 同构（每 64 行一个锚点，便于设备端块查找）：
    24B 头: "DCTV" | ver u16 | step u16 | count u32 | size u32 | 8B 保留
    每条锚点: u32 偏移 + 8B 块首词

用法：
    python tools/build_variants.py
    python tools/build_variants.py --no-filter     # 不要求原形在 dict.dat 里
"""

import argparse
import csv
import io
import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ECDICT = os.path.join(ROOT, "words", "ecdict.csv")
DICT_DAT = os.path.join(ROOT, "dict", "dict.dat")
OUT_DAT = os.path.join(ROOT, "dict", "variants.dat")
OUT_IDX = os.path.join(ROOT, "dict", "variants.idx")
SIM_DICT_DIR = os.path.join(ROOT, "sim", "sd", "dict")

STEP = 64
KEY_BYTES = 8
# 输出时的固定顺序（设备端按这个顺序展示）
PREFIX_ORDER = ["p", "d", "i", "3", "s", "r", "t"]


def parse_exchange(s):
    """exchange → {前缀: [形式]}"""
    out = {}
    for part in (s or "").split("/"):
        if ":" not in part:
            continue
        k, v = part.split(":", 1)
        k = k.strip()
        forms = [x.strip().lower() for x in v.split(",") if x.strip()]
        if k and forms:
            out[k] = forms
    return out


def load_dict_words(path):
    """只读 dict.dat 每行第一个字段，拿词集合"""
    words = set()
    if not os.path.exists(path):
        return words
    with io.open(path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            tab = line.find("\t")
            if tab > 0:
                words.add(line[:tab])
    return words


def collect(dict_words, no_filter):
    """遍历 ecdict，返回 {原形: [(前缀, [形式...]), ...]}"""
    base_forms = {}
    with io.open(ECDICT, "r", encoding="utf-8", errors="ignore", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            exch = (row.get("exchange") or "").strip()
            if not exch:
                continue
            ex = parse_exchange(exch)
            if not ex or "0" in ex:
                continue                    # 变形词，跳过（由原形行统一给出）

            w = (row.get("word") or "").strip().lower()
            if not w or " " in w:           # 短语没有变形，跳过
                continue
            if not no_filter and dict_words and w not in dict_words:
                continue

            forms = []
            for k in PREFIX_ORDER:
                vals = [x for x in ex.get(k, []) if x and x != w]
                if vals:
                    forms.append((k, vals))
            if forms:
                base_forms[w] = forms
    return base_forms


def write_outputs(base_forms):
    lines = []
    for w in sorted(base_forms, key=lambda s: s.encode("utf-8")):
        parts = []
        for k, vals in base_forms[w]:
            parts.append("%s:%s" % (k, ",".join(vals)))
        lines.append("%s\t%s" % (w, "|".join(parts)))

    blob = bytearray()
    offsets = []
    for i, ln in enumerate(lines):
        if i % STEP == 0:
            key = ln.split("\t", 1)[0].encode("utf-8")[:KEY_BYTES]
            offsets.append((len(blob), key))
        blob += ln.encode("utf-8") + b"\n"

    os.makedirs(os.path.dirname(OUT_DAT), exist_ok=True)
    with open(OUT_DAT, "wb") as f:
        f.write(blob)
    with open(OUT_IDX, "wb") as f:
        f.write(struct.pack("<4sHHII8s", b"DCTV", 1, STEP, len(lines), len(blob), b"\0" * 8))
        for off, key in offsets:
            f.write(struct.pack("<I", off))
            f.write(key + b"\0" * (KEY_BYTES - len(key)))

    # 模拟器目录也放一份，方便界面调试
    if os.path.isdir(os.path.dirname(SIM_DICT_DIR)):
        os.makedirs(SIM_DICT_DIR, exist_ok=True)
        for src, dst in ((OUT_DAT, os.path.join(SIM_DICT_DIR, "variants.dat")),
                         (OUT_IDX, os.path.join(SIM_DICT_DIR, "variants.idx"))):
            with open(src, "rb") as fi, open(dst, "wb") as fo:
                fo.write(fi.read())

    return lines, blob


def main():
    ap = argparse.ArgumentParser(description="生成 dict/variants.dat + variants.idx")
    ap.add_argument("--no-filter", action="store_true",
                    help="不要求原形在 dict.dat 里（默认只保留能查到的）")
    args = ap.parse_args()

    print("[1/3] 读 dict.dat 词集合 …")
    dict_words = load_dict_words(DICT_DAT)
    print("      dict.dat 里 %d 个词" % len(dict_words))

    print("[2/3] 解析 ecdict exchange …")
    base_forms = collect(dict_words, args.no_filter)
    print("      有变形的原形 %d 个" % len(base_forms))

    print("[3/3] 写 variants.dat / variants.idx …")
    lines, blob = write_outputs(base_forms)
    print("完成：%d 行，%.1f MB，索引 %d KB"
          % (len(lines), len(blob) / 1024.0 / 1024.0,
             os.path.getsize(OUT_IDX) / 1024))


if __name__ == "__main__":
    main()
