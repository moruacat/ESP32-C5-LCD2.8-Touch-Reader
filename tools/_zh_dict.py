# -*- coding: utf-8 -*-
"""CC-CEDICT 词库（中→英）的读取与转换，供 tools/build_dict.py 合并进 dict.dat。

数据源
------
`words/cedict_ts.u8`（CC-CEDICT，CC BY-SA 4.0；由 `words/cedict.zip` 解出，见 load()）

CC-CEDICT 每行形如::

    詞典 词典 [ci2 dian3] /dictionary (of Chinese)/CL:部[bu4],本[ben3]/

输出（与 ECDICT 侧完全同格式，5 字段 / 4 个 TAB）
-----------------------------------------------
word = 简体词（保持原样，含 CJK；纯 ASCII 的条目不要，会和英文侧撞大小写）
phonetic = 拼音（**带声调符号**，如 `cí diǎn`；glyph 由 font_ipa_14 提供）
definition = 英文释义，最多 3 个义项，`；` 相连，交叉引用（variant of / CL: / see …）剔除
flags = 空（CC-CEDICT 没有词频标签）
lemma = 繁体写法（与简体不同才填，UI 显示成「词典 → 詞典」）

注意：word 必须按 UTF-8 字节序排；调用方（build_dict.py）统一排序，这里只管内容。
"""

import os
import re
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORDS_DIR = os.path.join(ROOT, "words")
CEDICT_TXT = os.path.join(WORDS_DIR, "cedict_ts.u8")
CEDICT_ZIP = os.path.join(WORDS_DIR, "cedict.zip")
CEDICT_URL = "https://www.mdbg.net/chinese/export/cedict/cedict_1_0_ts_utf-8_mdbg.zip"

LINE_RE = re.compile(r"^(\S+)\s+(\S+)\s+\[([^\]]*)\]\s+/(.*)/$")

# 纯交叉引用 / 分类词标注：对「中→英」没有信息量，剔除后若只剩这些就整条丢弃
JUNK_SENSE = re.compile(
    r"^(CL:|variant of|old variant of|see |also written|also pr\.|abbr\. for|"
    r"Taiwan pr\.|erhua variant|Japanese variant|used in \S+$)"
)

SIMP_MAX_CHARS = 12      # word[40] 最多 13 个汉字，留 1 个余量
PHON_MAX_BYTES = 46      # phonetic[48]
DEF_MAX_BYTES = 420      # 和 ECDICT 侧一致（build_dict.py 的 DEF_MAX_BYTES）
DEF_MAX_SENSES = 3

TONE_MAP = {
    "a": "āáǎà", "o": "ōóǒò", "e": "ēéěè",
    "i": "īíǐì", "u": "ūúǔù", "ü": "ǖǘǚǜ",
}


def syllable(syl):
    """单个音节：`ni3` → `nǐ`，`lu:4` → `lǜ`，`de5` → `de`（轻声不着调）。"""
    s = syl.strip().lower().replace("u:", "ü").replace("v", "ü")
    m = re.match(r"^([a-zü]+)([1-5])?$", s)
    if not m:
        return s
    base, tone = m.group(1), m.group(2)
    if tone is None or tone == "5":
        return base
    tone = int(tone)

    if "a" in base:
        i = base.index("a")
    elif "o" in base:
        i = base.index("o")
    elif "e" in base:
        i = base.index("e")
    else:
        cand = [j for j, ch in enumerate(base) if ch in "iuü"]
        if not cand:
            return base
        i = cand[-1]                 # iu → 标 u；ui → 标 i
    ch = base[i]
    if ch not in TONE_MAP:
        return base
    return base[:i] + TONE_MAP[ch][tone - 1] + base[i + 1:]


def pinyin(py):
    """整串拼音：`ci2 dian3` → `cí diǎn`。"""
    return " ".join(syllable(p) for p in py.split())


def cut_bytes(s, limit):
    """按字节截断（不劈开 UTF-8 字符），超长补 `…`。"""
    b = s.encode("utf-8")
    if len(b) <= limit:
        return s, False
    out = b[:limit - 3]
    while out and (out[-1] & 0xC0) == 0x80:
        out = out[:-1]
    return out.decode("utf-8", "ignore") + "…", True


def _clean_def(raw, stats):
    senses = [s.strip() for s in raw.split("/") if s.strip()]
    if not senses:
        return ""
    keep = [s for s in senses if not JUNK_SENSE.match(s)]
    if not keep:
        stats["zh_ref_only"] = stats.get("zh_ref_only", 0) + 1
        return ""
    if len(keep) > DEF_MAX_SENSES:
        stats["zh_sense_cut"] = stats.get("zh_sense_cut", 0) + 1
        keep = keep[:DEF_MAX_SENSES]
    d = "；".join(keep)
    d = d.replace("\t", " ").replace("\r", " ").replace("\n", " ")
    return d


def load(stats=None):
    """读 CC-CEDICT，返回 `word -> {'phon','def','flags','lemma'}`（已去重、已截断）。"""
    if stats is None:
        stats = {}

    if not os.path.exists(CEDICT_TXT):
        if not os.path.exists(CEDICT_ZIP):
            raise SystemExit("缺 %s：先下载 %s 到 words/" % (CEDICT_TXT, CEDICT_URL))
        with zipfile.ZipFile(CEDICT_ZIP) as z:
            name = next(n for n in z.namelist() if n.endswith(".u8") or n.endswith(".txt"))
            data = z.read(name)
        with open(CEDICT_TXT, "wb") as f:
            f.write(data)
        print("      已从 %s 解出 %s" % (os.path.basename(CEDICT_ZIP),
                                        os.path.basename(CEDICT_TXT)))

    out = {}
    with open(CEDICT_TXT, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line[0] == "#":
                continue
            m = LINE_RE.match(line)
            if not m:
                stats["zh_bad_line"] = stats.get("zh_bad_line", 0) + 1
                continue
            trad, simp, py, raw_def = m.groups()

            if not any(ord(c) > 127 for c in simp):
                stats["zh_ascii_skip"] = stats.get("zh_ascii_skip", 0) + 1
                continue
            if len(simp) > SIMP_MAX_CHARS:
                stats["zh_long_skip"] = stats.get("zh_long_skip", 0) + 1
                continue

            d = _clean_def(raw_def, stats)
            if not d:
                stats["zh_no_def"] = stats.get("zh_no_def", 0) + 1
                continue
            d, cut = cut_bytes(d, DEF_MAX_BYTES)
            if cut:
                stats["zh_def_cut"] = stats.get("zh_def_cut", 0) + 1

            ph, _ = cut_bytes(pinyin(py), PHON_MAX_BYTES)
            lemma = trad if (trad != simp and len(trad) <= SIMP_MAX_CHARS) else ""
            lemma, _ = cut_bytes(lemma, 34)

            old = out.get(simp)
            if old is None:
                out[simp] = {"phon": ph, "def": d, "flags": "", "lemma": lemma}
            else:
                stats["zh_dup"] = stats.get("zh_dup", 0) + 1
                # 同一简体多个词条：并义项（仍受字节上限约束）
                merged = old["def"]
                if d not in merged:
                    merged, _ = cut_bytes(merged + "；" + d, DEF_MAX_BYTES)
                    old["def"] = merged
                if not old["lemma"] and lemma:
                    old["lemma"] = lemma
    return out