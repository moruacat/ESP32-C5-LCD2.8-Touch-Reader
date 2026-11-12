#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从人教版英语课本 PDF 的「Vocabulary in Each Unit」板块提取分单元生词，
直接生成设备端单词学习用的单元文件（`words/<词书名>/unit-XX.txt`）。

为什么不用 build_wordbook.py：课本整本提取会把课文、练习里的所有词都算进来，
生词表才是课本真正要求掌握的词，而且天然按 Unit 分好组。

解析规则（对应课本词表排版）：
    rough /rʌf/ adj. 崎岖的 p.2        → rough
    bring about 带来；引起 p.1          → bring about
    look up to sb 钦佩；仰慕 p.14       → look up to
      adj. 受伤的；负伤的 p.13          → 跳过（词性/释义续行）
    (= the Olympic Games)奥林匹克       → 跳过（同上）

用法：
    python tools/pdf_vocab.py 课本.pdf --name 九年级上册
"""

import argparse
import io
import os
import re

from pypdf import PdfReader

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DICT_DAT = os.path.join(ROOT, "dict", "dict.dat")
OUT_ROOT = os.path.join(ROOT, "words")

SECTION_START = "Vocabulary in Each Unit"
SECTION_ENDS = ("Vocabulary A-Z", "Vocabulary from Primary School", "Irregular Verbs")

UNIT_RE = re.compile(r"^Unit\s+(\d+)\s*$", re.IGNORECASE)
PAGE_NO_RE = re.compile(r"\s*p\.\s*\d+\s*$")

SKIP_PREFIX = (
    "adj.", "n.", "v.", "adv.", "pron.", "prep.", "conj.", "num.", "int.",
    "abbr.", "aux.", "modal", "pl.", "vt.", "vi.",
)
SKIP_LINE_PREFIX = ("（注", "(", "（", "仅供个人", "Vocabulary", "Unit")
CJK_RE = re.compile(r"[\u4e00-\u9fff\u3000-\u303f\uff00-\uffef]")


def read_pdf_text(path):
    reader = PdfReader(path)
    return [page.extract_text() or "" for page in reader.pages]


def locate_section(pages):
    """返回 (起始页号, 起始行号)；取「后面紧跟 Unit N」的那次出现（正文而非目录）"""
    for pi, text in enumerate(pages):
        lines = text.splitlines()
        for li, line in enumerate(lines):
            if SECTION_START not in line:
                continue
            for nx in lines[li + 1: li + 4]:
                if UNIT_RE.match(nx.strip()):
                    return pi, li
    return -1, -1


def clean_entry(line):
    """一行 → 词条（None 表示这行不是新词条）"""
    s = line.strip()
    if not s:
        return None
    for p in SKIP_LINE_PREFIX:
        if s.startswith(p):
            return None
    low = s.lower()
    for p in SKIP_PREFIX:
        if low.startswith(p):
            return None

    s = PAGE_NO_RE.sub("", s)

    # 截到第一个音标斜杠 / 中文 / 左括号
    cut = len(s)
    for i, ch in enumerate(s):
        if ch == "/" or ch == "(" or CJK_RE.match(ch):
            cut = i
            break
    head = s[:cut].strip()

    # 去掉残留括号内容
    head = re.sub(r"\([^)]*\)", "", head).strip()
    # 短语末尾的 sb / sth 占位符不参与查词
    head = re.sub(r"\s+(sb|sth)$", "", head, flags=re.IGNORECASE).strip()
    head = head.strip(" -—:：,，")

    if not head or not re.match(r"^[A-Za-z]", head):
        return None
    if len(head) > 38:
        return None
    return head


def parse_units(pages, start_page, start_line, end_marker_seen):
    """按 Unit N 切分；返回 [(unit_no, [word...]), ...]

    两道保险防止把书末的 Vocabulary A-Z 总表也算进来：
      - Unit 编号必须递增，出现「不递增的 Unit N」就停（总表后面按七上/七下分组，
        会再出现 Unit 1/3/4/6）
      - 单单元词数上限 MAX_PER_UNIT，超出的词条直接忽略（课本单元生词最多六七十）
    """
    MAX_PER_UNIT = 120
    units = []
    cur_no = None
    cur_words = []
    last_no = 0
    prev_word = ""
    asc_run = 0

    for pi in range(start_page, len(pages)):
        lines = pages[pi].splitlines()
        begin = start_line if pi == start_page else 0
        for line in lines[begin:]:
            s = line.strip()
            if any(s.startswith(x) for x in SECTION_ENDS):
                end_marker_seen[0] = True
                break

            m = UNIT_RE.match(s)
            if m:
                no = int(m.group(1))
                if cur_no is not None:
                    if no <= last_no:       # 编号不递增 → 进入附录总表
                        units.append((cur_no, cur_words))
                        cur_no = None
                        end_marker_seen[0] = True
                        break
                    units.append((cur_no, cur_words))
                    last_no = cur_no
                cur_no = no
                cur_words = []
                prev_word = ""
                asc_run = 0
                continue

            if len(cur_words) >= MAX_PER_UNIT:
                continue

            w = clean_entry(line)
            if w and cur_no is not None:
                lw = w.lower()

                # Vocabulary A-Z 总表以单字母词条 "a" 开头；生词表不会把 a 当生词
                if lw == "a" and len(cur_words) > 20:
                    end_marker_seen[0] = True
                    break

                # 兜底：总表是严格字母升序，连续 12 个升序也收手
                if prev_word and lw >= prev_word:
                    asc_run += 1
                else:
                    asc_run = 0
                prev_word = lw
                if asc_run >= 12:
                    end_marker_seen[0] = True
                    break

                if lw not in [x.lower() for x in cur_words]:
                    cur_words.append(w)
        if end_marker_seen[0]:
            break

    if cur_no is not None:
        units.append((cur_no, cur_words))
    return units


def load_dict_words():
    words = set()
    if not os.path.exists(DICT_DAT):
        return words
    with io.open(DICT_DAT, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            tab = line.find("\t")
            if tab > 0:
                words.add(line[:tab])
    return words


def main():
    ap = argparse.ArgumentParser(description="课本 PDF 词汇表 → 分单元单词文件")
    ap.add_argument("pdf", help="课本 PDF 路径")
    ap.add_argument("--name", default="", help="词书名（默认取 PDF 文件名）")
    ap.add_argument("--keep-missing", action="store_true",
                    help="保留词典未收录的词（默认过滤）")
    args = ap.parse_args()

    print("[1/4] 提取 PDF 文本 …")
    pages = read_pdf_text(args.pdf)
    print("      %d 页" % len(pages))

    print("[2/4] 定位词汇表 …")
    pi, li = locate_section(pages)
    if pi < 0:
        print("没有找到「%s」板块，确认 PDF 是这套课本的教师/学生用书" % SECTION_START)
        return
    print("      从第 %d 页开始" % (pi + 1))

    print("[3/4] 解析单元 …")
    ended = [False]
    units = parse_units(pages, pi, li, ended)
    dict_words = load_dict_words()

    name = args.name or os.path.splitext(os.path.basename(args.pdf))[0]
    safe = re.sub(r'[\\/:*?"<>|]', "_", name).strip() or "wordbook"
    out_dir = os.path.join(OUT_ROOT, safe)
    os.makedirs(out_dir, exist_ok=True)
    for fn in os.listdir(out_dir):
        if re.match(r"unit-\d+\.txt$", fn) or fn == "info.txt":
            os.remove(os.path.join(out_dir, fn))

    total = 0
    missing = 0
    print("[4/4] 写单元文件 …")
    for i, (no, words) in enumerate(units, 1):
        kept = []
        for w in words:
            if args.keep_missing or not dict_words or w.lower() in dict_words:
                kept.append(w.lower())
            else:
                missing += 1
        if not kept:
            continue
        path = os.path.join(out_dir, "unit-%02d.txt" % i)
        with io.open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(kept) + "\n")
        total += len(kept)
        print("      Unit %-2d → unit-%02d.txt  %d 词%s"
              % (no, i, len(kept), "" if no == i else "（课本 Unit %d）" % no))

    with io.open(os.path.join(out_dir, "info.txt"), "w",
                 encoding="utf-8", newline="\n") as f:
        f.write("name=%s\n" % safe)
        f.write("units=%d\n" % len(units))
        f.write("words=%d\n" % total)
        f.write("unit_size=0\n")

    print("完成：%s，%d 个单元，%d 个词（词典未收录过滤掉 %d 个）"
          % (out_dir, len(units), total, missing))
    print("把整个目录拷到 SD 卡的 /words/ 下即可。")


if __name__ == "__main__":
    main()
