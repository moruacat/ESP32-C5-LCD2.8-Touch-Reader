#!/usr/bin/env python3
"""把 CC-CEDICT 转成设备端的中英双向词典。

输入：words/cedict.txt.gz（CC-CEDICT 官方发布包，UTF-8）
输出：
  dict/zhdict.dat       中文词 \t 拼音 \t 英文释义 \t flags \t lemma
  dict/zhdict.idx       主索引（按中文词 UTF-8 排序，每 64 行一个锚点）
  dict/zhdict_py.idx    拼音索引（无声调，按拼音排序，每 64 行一个锚点）
  dict/zhdict_report.txt

格式与 srv_dict_zh.c 的读取代码一一对应，改动必须两边同步。
"""
import gzip
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "words" / "cedict.txt.gz"
OUT_DIR = ROOT / "dict"
OUT_DAT = OUT_DIR / "zhdict.dat"
OUT_IDX = OUT_DIR / "zhdict.idx"
OUT_PY = OUT_DIR / "zhdict_py.idx"
OUT_REPORT = OUT_DIR / "zhdict_report.txt"

BLOCK = 64              # 每 64 行一个锚点
HEAD_BYTES = 8          # 锚点里记录块首词的前 8 字节
MAX_DEF = 150           # 释义最多保留这么多字节
MAX_ITEMS = 3           # 最多保留几个英文义项

LINE_RE = re.compile(r"^(.*?) (.*?) \[([^\]]*)\] /(.*)/")


def strip_tones(py: str) -> str:
    """chuan2 tong3 -> chuan tong"""
    out = []
    for tok in py.split():
        tok = "".join(ch for ch in tok if not ch.isdigit())
        if tok:
            out.append(tok)
    return " ".join(out)


def is_gb2312(s: str) -> bool:
    try:
        s.encode("gb2312")
        return True
    except UnicodeEncodeError:
        return False


def clean_def(raw: str) -> str:
    parts = [p.strip() for p in raw.split("/") if p.strip()]
    text = "; ".join(parts[:MAX_ITEMS])
    text = text.replace("\t", " ").replace("\n", " ")
    if len(text.encode("utf-8")) > MAX_DEF:
        cut = text.encode("utf-8")[:MAX_DEF].decode("utf-8", "ignore")
        text = cut + "…"
    return text


def main() -> int:
    if not SRC.exists():
        print(f"缺少输入文件：{SRC}", file=sys.stderr)
        return 1

    rows = []           # (word, pinyin, def)
    seen = set()
    stats = {"raw": 0, "gb2312": 0, "dup": 0, "len": 0}

    with gzip.open(SRC, "rt", encoding="utf-8") as fp:
        for line in fp:
            if line.startswith("#"):
                continue
            m = LINE_RE.match(line.rstrip())
            if not m:
                continue
            stats["raw"] += 1
            simp = m.group(2)
            if not (1 <= len(simp) <= 4) or not is_gb2312(simp):
                stats["len"] += 1
                continue
            if simp in seen:
                stats["dup"] += 1
                continue
            seen.add(simp)
            stats["gb2312"] += 1
            py = strip_tones(m.group(3))
            rows.append((simp, py, clean_def(m.group(4))))

    # 主索引按中文词排序；拼音索引按无声调拼音排序
    rows.sort(key=lambda r: r[0])
    by_py = sorted(rows, key=lambda r: (r[1], r[0]))

    OUT_DIR.mkdir(parents=True, exist_ok=True)

    # ---- 主数据 ----
    offsets = []        # 每行的文件偏移
    out = bytearray()
    for word, py, df in rows:
        offsets.append(len(out))
        out += f"{word}\t{py}\t{df}\t0\t\n".encode("utf-8")
    OUT_DAT.write_bytes(out)

    # ---- 主索引：count + 每个锚点 12 字节（正文偏移 u32, 首词前 8 字节）----
    def write_index(path, ordered, key_fn):
        anchors = []
        for i in range(0, len(ordered), BLOCK):
            word, py, _ = ordered[i]
            key = key_fn(word, py).encode("utf-8")[:HEAD_BYTES]
            anchors.append((offsets[i], key.ljust(HEAD_BYTES, b"\0")))
        buf = bytearray()
        buf += struct.pack("<II", len(ordered), len(anchors))
        for line_no, key in anchors:
            buf += struct.pack("<I", line_no) + key
        path.write_bytes(buf)
        return len(anchors)

    n_main = write_index(OUT_IDX, rows, lambda w, p: w)
    n_py = (len(by_py) + BLOCK - 1) // BLOCK

    # ---- 拼音索引 ----
    # 布局：header(8) + 锚点(16 字节/个：拼音前 8 字节 + 该块在偏移表里的起始序号)
    #       + 偏移表(count * u32，正文文件偏移)
    # 这样拼音查找只需常驻锚点（约 27KB），块内 64 个偏移按需 fseek 读。
    row_to_no = {id(r): i for i, r in enumerate(rows)}
    line_nos = [offsets[row_to_no[id(r)]] for r in by_py]
    py_buf = bytearray()
    py_buf += struct.pack("<II", len(by_py), n_py)
    for i in range(0, len(by_py), BLOCK):
        key = by_py[i][1].encode("utf-8")[:HEAD_BYTES].ljust(HEAD_BYTES, b"\0")
        py_buf += key + struct.pack("<II", i, 0)
    for no in line_nos:
        py_buf += struct.pack("<I", no)
    OUT_PY.write_bytes(py_buf)

    size_dat = OUT_DAT.stat().st_size
    report = [
        "CC-CEDICT -> zhdict",
        f"raw entries      : {stats['raw']}",
        f"kept (GB2312+len): {stats['gb2312']}",
        f"dropped (len)    : {stats['len']}",
        f"dropped (dup)    : {stats['dup']}",
        f"dat              : {size_dat} bytes ({size_dat/1024:.0f} KB)",
        f"main anchors     : {n_main}",
        f"pinyin anchors   : {n_py}",
        f"index bytes      : {OUT_IDX.stat().st_size} + {OUT_PY.stat().st_size}",
    ]
    OUT_REPORT.write_text("\n".join(report) + "\n", encoding="utf-8")
    print("\n".join(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
