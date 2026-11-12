#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""分析一个文本文件（课本 / 课文 / 字幕 / 词表），提取其中所有英文单词，
生成设备端「单词学习」用的单元文件。

输出目录：`words/<词书名>/`
    unit-01.txt     每行一个单词（小写，已去重、去停用词）
    unit-02.txt
    info.txt        name / units / words / unit_size

切分规则：
    默认        每 --unit 个词一单元（默认 30）
    --by-heading 按文本里的章节标记切：`Unit 3` / `Lesson 5` / `Chapter 2` / `# 标题`
                 切完如果某块超过 --unit*2 词，再按 --unit 拆分

过滤：
    - 长度 < 2 的词、纯数字
    - 常见停用词（the/a/is/…）
    - 默认只保留 dict.dat 里能查到的词（--no-filter 关掉）

用法：
    python tools/build_wordbook.py 课本.txt
    python tools/build_wordbook.py 课本.txt --name 新概念2 --unit 30 --by-heading
    python tools/build_wordbook.py ./txt目录 --name 综合 --no-filter
"""

import argparse
import io
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DICT_DAT = os.path.join(ROOT, "dict", "dict.dat")
OUT_ROOT = os.path.join(ROOT, "words")

WORD_RE = re.compile(r"[A-Za-z][A-Za-z'\-]*")
HEAD_RE = re.compile(r"^\s*(?:#{1,3}\s*|\**\s*)(?:unit|lesson|chapter|part)\s*\d+",
                     re.IGNORECASE)
MD_HEAD_RE = re.compile(r"^\s*#{1,3}\s+\S")

# 只挡最基础的虚词；真正的生词过滤交给词典收录判断
STOP_WORDS = set("""
a an the and or but if because so than then that this these those there here
i you he she it we they me him her us them my your his its our their
am is are was were be been being do does did done doing have has had having
will would shall should can could may might must
of in on at by for with from to into onto about over under between among
as not no nor too very just only also even still yet
up down out off again further once
what which who whom whose when where why how
all any both each few more most other some such own same
s t re ve ll d m
""".split())


def read_text(path):
    """按 UTF-8 → GBK 顺序尝试解码"""
    with open(path, "rb") as f:
        raw = f.read()
    for enc in ("utf-8-sig", "utf-8", "gbk"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", errors="ignore")


def load_dict_words():
    """dict.dat 的词集合（只读第一个字段）"""
    words = set()
    if not os.path.exists(DICT_DAT):
        return words
    with io.open(DICT_DAT, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            tab = line.find("\t")
            if tab > 0:
                words.add(line[:tab])
    return words


def normalize(token):
    """小写、去首尾的引号/连字符"""
    t = token.lower().strip("'-")
    return t


def extract_words(text, dict_words, no_filter):
    """按出现顺序提取、去重"""
    seen = set()
    out = []
    for m in WORD_RE.finditer(text):
        w = normalize(m.group(0))
        if len(w) < 2:
            continue
        if w in STOP_WORDS or w in seen:
            continue
        if not no_filter and dict_words and w not in dict_words:
            continue
        seen.add(w)
        out.append(w)
    return out


def split_by_heading(text):
    """按章节标记切块；没有标记时返回整篇"""
    blocks = []
    cur = []
    for line in text.splitlines():
        if HEAD_RE.match(line) or MD_HEAD_RE.match(line):
            if cur:
                blocks.append("\n".join(cur))
                cur = []
        cur.append(line)
    if cur:
        blocks.append("\n".join(cur))
    return blocks if blocks else [text]


def chunk(seq, n):
    for i in range(0, len(seq), n):
        yield seq[i:i + n]


def write_book(name, units, unit_size):
    safe = re.sub(r'[\\/:*?"<>|]', "_", name).strip() or "wordbook"
    out_dir = os.path.join(OUT_ROOT, safe)
    os.makedirs(out_dir, exist_ok=True)

    # 清掉上一次生成的旧单元，避免残留
    for fn in os.listdir(out_dir):
        if re.match(r"unit-\d+\.txt$", fn) or fn == "info.txt":
            os.remove(os.path.join(out_dir, fn))

    total = 0
    for i, words in enumerate(units, 1):
        total += len(words)
        path = os.path.join(out_dir, "unit-%02d.txt" % i)
        with io.open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(words) + "\n")

    with io.open(os.path.join(out_dir, "info.txt"), "w",
                 encoding="utf-8", newline="\n") as f:
        f.write("name=%s\n" % safe)
        f.write("units=%d\n" % len(units))
        f.write("words=%d\n" % total)
        f.write("unit_size=%d\n" % unit_size)

    return safe, out_dir, total


def main():
    ap = argparse.ArgumentParser(description="文本 → 单词学习单元文件")
    ap.add_argument("input", help="文本文件或目录")
    ap.add_argument("--name", default="", help="词书名（默认取文件名）")
    ap.add_argument("--unit", type=int, default=30, help="每单元词数（默认 30）")
    ap.add_argument("--by-heading", action="store_true",
                    help="按 Unit/Lesson/Chapter/# 标题切分")
    ap.add_argument("--no-filter", action="store_true",
                    help="不要求单词在 dict.dat 里")
    args = ap.parse_args()

    files = []
    if os.path.isdir(args.input):
        for fn in sorted(os.listdir(args.input)):
            p = os.path.join(args.input, fn)
            if os.path.isfile(p) and fn.lower().endswith((".txt", ".md", ".srt")):
                files.append(p)
    else:
        files.append(args.input)
    if not files:
        print("没有找到可分析的文本文件")
        return

    print("[1/3] 读词库索引 …")
    dict_words = load_dict_words()
    print("      dict.dat 里 %d 个词" % len(dict_words))

    print("[2/3] 提取单词 …")
    words = []
    seen = set()
    heading_units = []          # --by-heading 时按块收集
    for path in files:
        text = read_text(path)
        if args.by_heading:
            for b in split_by_heading(text):
                ws = extract_words(b, dict_words, args.no_filter)
                uniq = []
                for w in ws:
                    if w not in seen:
                        seen.add(w)
                        uniq.append(w)
                if uniq:
                    heading_units.append(uniq)
        got = extract_words(text, dict_words, args.no_filter)
        for w in got:
            if w not in seen:
                seen.add(w)
                words.append(w)
    print("      去重后 %d 个词（%d 个文件）" % (len(words), len(files)))
    if not words:
        print("没有提取到有效单词，检查输入文件或加 --no-filter")
        return

    print("[3/3] 切分单元 …")
    if args.by_heading and heading_units:
        units = []
        for ws in heading_units:
            if len(ws) > args.unit * 2:
                units.extend(list(chunk(ws, args.unit)))
            else:
                units.append(ws)
    else:
        units = list(chunk(words, args.unit))

    name = args.name or os.path.splitext(os.path.basename(files[0]))[0]
    safe, out_dir, total = write_book(name, units, args.unit)
    print("完成：%s，%d 个单元，%d 个词" % (out_dir, len(units), total))
    print("把整个目录拷到 SD 卡的 /words/ 下即可在设备上选择学习。")


if __name__ == "__main__":
    main()
