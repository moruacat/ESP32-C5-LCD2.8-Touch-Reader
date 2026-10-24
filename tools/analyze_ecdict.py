# -*- coding: utf-8 -*-
"""分析 ecdict.csv：音标字符集合、翻译汉字集合、字段概况。输出 UTF-8 文本到指定文件。"""
import io
import csv
import unicodedata
from collections import Counter

SRC = r"words\ecdict.csv"
OUT = r"tools\_analyze_out.txt"

phon_allow = set("ˈˌːˑ͜͡əɚɜɝɐɪʊʏɛœʌɔɒɑæɨʉʃʒʧʤθðçɡŋɲɹɾʔʰʲʷⁿ")

buf = []

with io.open(SRC, encoding="utf-8-sig", newline="") as f:
    reader = csv.reader(f)
    header = next(reader)
    total = 0
    phon_no_bracket = 0
    phon_empty = 0
    phon_chars = Counter()
    phon_outside = Counter()
    wanzi = Counter()
    cols_dist = Counter()
    for row in reader:
        total += 1
        cols_dist[len(row)] += 1
        if len(row) < 12:
            continue
        word = row[0]
        phonetic = row[1]
        translation = row[3]
        if not phonetic:
            phon_empty += 1
        else:
            if not (phonetic.startswith("/") and phonetic.endswith("/")):
                phon_no_bracket += 1
            for ch in phonetic:
                phon_chars[ch] += 1
                if ch not in phon_allow and ch not in "/" and ch not in " ":
                    phon_outside[ch] += 1
        for ch in translation:
            if unicodedata.category(ch) == "Lo":
                wanzi[ch] += 1

buf.append("header: %s" % (header,))
buf.append("total data lines: %d" % total)
buf.append("column count distribution: %s" % dict(cols_dist))
buf.append("phonetic empty: %d" % phon_empty)
buf.append("phonetic not wrapped in //: %d" % phon_no_bracket)
buf.append("")
buf.append("=== phonetic char frequency (excl space) ===")
for ch, n in phon_chars.most_common(120):
    if ch in " \t":
        rep = "<space>"
    else:
        rep = ch
    buf.append("U+%04X %s : %d" % (ord(ch), rep, n))
buf.append("")
buf.append("=== phonetic chars OUTSIDE whitelist (excl /) ===")
for ch, n in phon_outside.most_common(300):
    try:
        name = unicodedata.name(ch)
    except ValueError:
        name = "?"
    buf.append("U+%04X %s (%s) : %d" % (ord(ch), ch, name, n))
buf.append("")
buf.append("=== distinct Han-char count in translation col ===")
buf.append("distinct Lo chars: %d" % len(wanzi))

with io.open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(buf))
print("done, total=%d" % total)
print("wrote", OUT)