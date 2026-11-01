# -*- coding: utf-8 -*-
"""生成设备端离线词库：`dict/dict.dat`（主表）+ `dict/dict.idx`（稀疏索引）。

数据源
------
- `words/ecdict.csv`（MIT 授权，个人自用；770k 行，66MB）→ 英→中
- `words/cedict_ts.u8`（CC-CEDICT，CC BY-SA 4.0，12.5 万条，见 tools/_zh_dict.py）→ 中→英
- `tools/_manual_defs.py` 里的人工释义（786 条，最高优先）与 REMOVE（5 条）

两侧合并进**同一张表**（同一份索引/同一套查找代码），中文词按 UTF-8 字节序排在
ASCII 词之后，所以查词页不用分流：输入英文查英→中，输入中文查中→英。

产物格式（详见 .trae/documents/离线词典升级与词级直译.md）
----------------------------------------------------------
dict.dat 每行 5 字段 / 4 个 TAB：`word \\t phonetic \\t definition \\t flags \\t lemma`
  - word 全小写，`give up` 这类短语也收；按 UTF-8 字节序升序
  - definition 保证不含 TAB/CR/LF（换行转 `；`），超长截到 420 字节 + `…`
  - flags 单字符一标签：4=CET4 6=CET6 K=考研 G=高考 Z=中考 T=TOEFL I=IELTS
    R=GRE O=Oxford3000 C<1-5>=Collins 星级
  - lemma = 本词的原形（可空）；变形词若不在表内会**追加一行**指向原词

dict.idx 头 24B（magic "DCTX" | ver u16 | step u16 | count u32 | size u32 | 8B 保留）
+ 锚点 12B/项（u32 文件偏移 | 块首词前 8 字节，'\\0' 补齐），每 step=64 行一个锚点。

用法
----
    python tools/build_dict.py                  # 全流程（选词→生成→索引→拷 sim→报告）
    python tools/build_dict.py --only-data      # 只出数据，不做缺字/拷贝
    python tools/build_dict.py --update-hanzi   # 把缺字追加进 font/hanzi.txt
    python tools/build_dict.py --no-copy        # 不写 sim/sd/dict
    python tools/build_dict.py --no-zh          # 不合并 CC-CEDICT（只出英→中）
    python tools/build_dict.py --limit 3000     # 小样本调试（只取前 N 条）
"""

import argparse
import csv
import io
import os
import re
import shutil
import struct
import sys
import unicodedata
from collections import Counter, OrderedDict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _manual_defs as MD          # noqa: E402  人工释义表（必须保留）
import _zh_dict as ZD              # noqa: E402  CC-CEDICT 中→英（合并进同一张表）

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EC_CSV = os.path.join(ROOT, "words", "ecdict.csv")
HANZI_TXT = os.path.join(ROOT, "font", "hanzi.txt")
TTF = os.path.join(ROOT, "font", "HarmonyOS_Sans_SC_Medium.ttf")
OUT_DAT = os.path.join(ROOT, "dict", "dict.dat")
OUT_IDX = os.path.join(ROOT, "dict", "dict.idx")
SIM_DICT_DIR = os.path.join(ROOT, "sim", "sd", "dict")
REPORT = os.path.join(ROOT, "tools", "build_report.txt")
OLD_CSVS = [os.path.join(ROOT, "dict", "dict.csv"),
            os.path.join(SIM_DICT_DIR, "dict.csv")]

# ---------------- 可调旋钮 ----------------
STEP = 64               # 索引粒度：每多少行一个锚点
EXTRA_MAX_LEN = 8       # 「无任何词频/标签信号」的短词最大长度（8 → 约 15.7 万条）
DEF_MAX_BYTES = 420     # 释义截断上限（字节）
MAX_ITEMS_PER_POS = 5   # 每个词性最多留几个义项
KEY_BYTES = 8           # 锚点里存块首词的前几字节

# ---------------- 音标白名单（沿用旧流水线） ----------------
PHON_IPA = set("ˈˌːˑ͜͡əɚɜɝɐɪʊʏɛœʌɔɒɑæɨʉʃʒʧʤθðçɡŋɲɹɾʔʰʲʷⁿ")

POS_MAP = {
    'n': 'n.', 'v': 'v.', 'vt': 'v.', 'vi': 'v.', 'adj': 'adj.', 'a': 'adj.',
    'ad': 'adv.', 'adv': 'adv.', 'prep': 'prep.', 'conj': 'conj.',
    'pron': 'pron.', 'num': 'num.', 'int': 'int.', 'art': 'art.',
    'abbr': 'abbr.', 'aux': 'v.', 'interj': 'int.', 'det': '', 'pl': 'n.',
}
POS_ORDER = ['n.', 'v.', 'adj.', 'adv.', 'prep.', 'conj.', 'pron.', 'num.',
             'int.', 'art.', 'abbr.']
POS_KEY = {p: i for i, p in enumerate(POS_ORDER)}

WORD_RE = re.compile(r"^[a-z][a-z'\- ]*$")
TAG_CODE = OrderedDict([
    ("cet4", "4"), ("cet6", "6"), ("ky", "K"), ("gk", "G"), ("zk", "Z"),
    ("toefl", "T"), ("ielts", "I"), ("gre", "R"),
])
KEEP_PUNCT = '，。；、（）《》·—…！？：'
CJK_RE = re.compile(r"[\u4e00-\u9fff]")

#: 短语里的占位词（`credit sb with sth` 这种不是给人看的词条）
PLACEHOLDER_TOKENS = frozenset((
    "sb", "sbs", "sth", "sths", "one", "ones", "someone", "something",
    "somebody", "pl", "abbr", "esp", "usu", "etc",
))
#: 介词 / 副词小品词：短语里带上它们才像「动词短语 / 介词短语」而不是名词堆砌
PARTICLES = frozenset((
    "up", "down", "out", "in", "on", "off", "for", "with", "to", "at", "by",
    "of", "into", "over", "away", "back", "through", "about", "after",
    "before", "from", "against", "upon", "as", "than", "across", "along",
    "around", "behind", "between", "beyond", "during", "except", "inside",
    "like", "near", "past", "since", "toward", "towards", "under", "until",
    "without", "within", "among", "beside", "onto", "amid", "ahead", "apart",
))
#: 短语首词允许是「词频前 N 的常用词」（给 give up / put off 这类没有词频信号的
#: 常用短语留活路；反之 `diospyros kaki` 这种首词生僻的一律不收）
PHRASE_HEAD_TOP = 3000
PHRASE_MAX_WORDS = 3

# 统计
stats = Counter()
used_chars = set()      # 释义里出现过的非 ASCII 字符
oob_chars = set()       # 其中不在 hanzi.txt 里的
no_glyph_chars = set()  # 连源字体都没有的（会被丢掉）
long_defs = []          # 被截断的词


# ===================== 小工具（多数沿用 _step5_generate.py） =====================

def norm_pos(p):
    """词性缩写归一为带点形式，如 'v.'；复合词性取优先级最高的一个。"""
    p = (p or '').strip().strip('.').lower()
    if not p:
        return ''
    best = ''
    for q in re.split(r'[&,，\s]+', p):
        q = q.strip().strip('.').lower()
        mapped = POS_MAP.get(q, '')
        if mapped and mapped in POS_KEY:
            if not best or POS_KEY[mapped] < POS_KEY[best]:
                best = mapped
    return best


def clean_def(s):
    """清理义项文本：保留汉字（字库外也先留着，最后统一补字/丢弃）、中文标点、
    数字与连字符；去掉 ASCII 单词与杂散符号。"""
    if s is None:
        return ''
    s = s.replace('\u200b', '').replace('\t', ' ')
    s = s.replace('\\n', '；').replace('\\', '')
    s = re.sub(r'[A-Za-z][A-Za-z\-]*', '', s)

    out = []
    for ch in s:
        if ord(ch) > 127:
            if unicodedata.category(ch) == 'Lo':
                out.append(ch)
            elif ch in KEEP_PUNCT:
                out.append(ch)
            continue
        # ASCII
        if ch == '(':
            out.append('（')
        elif ch == ')':
            out.append('）')
        elif ch == ';':
            out.append('；')
        elif ch == ',':
            out.append('，')
        elif ch.isdigit() or ch == '-':
            out.append(ch)

    s = ''.join(out)
    # 派生词的「（dictionary 的名词复数）」这类纯语法注释：里面的英文词被去掉后
    # 只剩「（；的复数）」这种空壳，整段丢掉
    s = re.sub(r'（[^）]*?(?:复数|过去式|过去分词|现在分词|第三人称|比较级|最高级)[^）]*?）', '', s)
    s = re.sub(r'（\s*[；，、]*\s*）', '', s)          # 空括号
    s = re.sub(r'[；，、]+\s*（', '（', s)
    s = re.sub(r' +', ' ', s)
    s = re.sub(r'[；\s]+([，。])', r'\1', s)
    s = re.sub(r'[；，、]{2,}', '；', s)
    return s.strip().strip('；，')


def split_items(text):
    """把一段翻译拆成最小义项。"""
    out = []
    junk = {'的过去式', '的过去分词', '的过去式和过去分词', '的过去式和现在分词'}
    for seg in re.split(r'[；;。，,、\s]+', text):
        seg = clean_def(seg).strip().strip('；，、 ')
        if not seg or seg in junk:
            continue
        # 只剩括号/标点的碎片（语法注释被拆散后的残渣）没意义
        if not re.search(r'[\u4e00-\u9fff0-9A-Za-z]', seg):
            continue
        out.append(seg)
    return out


def clean_phon_ec(s):
    """ecdict 旧式音标 → 标准 IPA（沿用旧流水线的清洗规则）。"""
    s = s or ''
    s = s.replace('\u200b', '').replace('\xa0', ' ')
    s = s.replace('ә', 'ə').replace('є', 'e').replace('ε', 'e').replace('ɛ', 'e')
    s = s.replace('ĭ', 'ɪ').replace('ū', 'uː').replace('ˊ', 'ˈ').replace('。', '')
    s = s.replace("'", 'ˈ')
    s = re.sub(r':', 'ː', s)
    s = re.sub(r'(?<=\w),(?=\w)', 'ˌ', s)
    s = re.sub(r',', ' ', s)
    s = re.sub(r'\s+', ' ', s)
    s = re.sub(r'\s*\([^)]*\)\s*', '', s)
    s = re.sub(r'\b(?:weak|strong)\s*form\b', '', s, flags=re.I)
    return re.sub(r'\s+', '', s)


def phon_ok(s):
    for ch in s:
        if ord(ch) > 127 and ch not in PHON_IPA:
            return False
    return True


def cut_bytes(s, limit):
    """按字节截断，不切坏 UTF-8；返回 (新串, 是否截断)。"""
    b = s.encode('utf-8')
    if len(b) <= limit:
        return s, False
    b = b[:limit]
    while b:
        try:
            return b.decode('utf-8') + '…', True
        except UnicodeDecodeError:
            b = b[:-1]
    return '', True


# ===================== 选词 =====================

def has_usable_def(translation):
    """翻译里至少有一行是「给人看的中文」（不是 `[医] …` 这种专业标签行）。"""
    for ln in (translation or '').replace('\\n', '\n').split('\n'):
        ln = ln.strip()
        if ln and not ln.startswith('[') and CJK_RE.search(ln):
            return True
    return False


def phrase_ok(w, translation, freq_top):
    """无常用度信号的「多词短语」是否值得收：首词常用 + 带小品词 + 无占位词。"""
    toks = w.split()
    if len(toks) > PHRASE_MAX_WORDS:
        return False
    if any(("'" in t) or (t in PLACEHOLDER_TOKENS) for t in toks):
        return False
    if toks[0] not in freq_top:
        return False
    if not any(t in PARTICLES for t in toks[1:]):
        return False
    return has_usable_def(translation)


def row_keep(row, freq_top):
    """选词规则：有中文释义 + 词形合法，且满足下面之一：

    1. 有「常用度信号」：tag / collins / oxford / bnc / frq（短语也照收）
    2. 单词且带 exchange（变位词，如 runs/ran）
    3. 单词、纯字母、有音标、长度 <= EXTRA_MAX_LEN（兜底的常用短词）
    4. 多词短语：首词在词频前 PHRASE_HEAD_TOP、含介词/副词小品词、释义可用

    为什么这么绕：ECDICT 里 `a-bombs`/`a-coming` 这类派生垃圾有 1.2 万条（全都
    带 exchange），而 `give up`/`put off`/`in front of` 这些真短语**没有任何
    词频/标签信号** —— 所以含连字符的一律只认信号 1，含空格的用规则 4 兜。
    """
    w = (row.get('word') or '').strip().lower()
    if not w or not WORD_RE.match(w):
        return None
    if not (row.get('translation') or '').strip():
        return None

    def num(v):
        try:
            return int(v or 0)
        except ValueError:
            return 0

    tag = (row.get('tag') or '').strip()
    collins = num(row.get('collins'))
    oxford = num(row.get('oxford'))
    bnc = num(row.get('bnc'))
    frq = num(row.get('frq'))
    exch = (row.get('exchange') or '').strip()

    if tag or collins > 0 or oxford > 0 or bnc > 0 or frq > 0:
        return w
    if '-' in w:
        return None                     # 连字符派生词：没信号一律不收
    if ' ' in w:
        return w if phrase_ok(w, row.get('translation'), freq_top) else None
    if exch:
        return w
    if w.isalpha() and (row.get('phonetic') or '').strip() and len(w) <= EXTRA_MAX_LEN:
        return w
    return None


def load_freq_top(top=PHRASE_HEAD_TOP):
    """先扫一遍 ecdict，取词频最高的前 N 个单词（给短语首词做判据）。"""
    pairs = []
    with io.open(EC_CSV, encoding='utf-8', newline='') as f:
        for row in csv.DictReader(f):
            w = (row.get('word') or '').strip().lower()
            if (' ' in w) or ('-' in w) or not w.isalpha():
                continue
            try:
                fq = int(row.get('frq') or 0)
            except ValueError:
                fq = 0
            if fq > 0:
                pairs.append((fq, w))
    pairs.sort()
    return {w for _, w in pairs[:top]}


def read_ecdict(freq_top, limit=None):
    """流式读 ecdict，返回 {word: row}（已按选词规则过滤、去重、去掉 REMOVE）。"""
    picked = OrderedDict()
    with io.open(EC_CSV, encoding='utf-8', newline='') as f:
        rd = csv.DictReader(f)
        for row in rd:
            w = row_keep(row, freq_top)
            if w is None or w in picked or w in MD.REMOVE:
                continue
            picked[w] = row
            if limit and len(picked) >= limit:
                break
    return picked


# ===================== 释义 =====================

def def_from_ec(translation):
    """ecdict 的 translation → [(pos, [义项])]；pos 为空串表示没标词性。"""
    tr = (translation or '').replace('\\n', '\n').replace('\\', '')
    groups = OrderedDict()
    for ln in tr.split('\n'):
        ln = ln.strip()
        if not ln or ln.startswith('['):
            continue
        m = re.match(r'^([a-z]+\.?)\s*(.*)$', ln, re.I)
        if m:
            pos = norm_pos(m.group(1))
            rest = m.group(2)
        else:
            pos = ''
            rest = ln
        rest = re.sub(r'\[[^\]]*\]', '', rest)
        # 整段丢掉「（dictionary 的名词复数）」这类纯语法注释（在拆词之前处理，
        # 否则会被空格拆成两半、只剩「（」这种残渣）
        rest = re.sub(r'[（(][^）)]*?(?:复数|过去式|过去分词|现在分词|第三人称'
                      r'|比较级|最高级)[^）)]*?[）)]', '', rest)
        for seg in split_items(rest):
            if pos not in POS_KEY:
                pos = ''
            items = groups.setdefault(pos, [])
            if seg not in items and len(items) < MAX_ITEMS_PER_POS:
                items.append(seg)
    return [(p, its) for p, its in groups.items() if its]


def build_definition(word, row):
    """人工表最高优先；否则用 ecdict 的 translation 按词性拼接。"""
    if word in MD.MANUAL:
        text = MD.MANUAL[word].replace('\t', ' ').replace('\n', '；').replace('\r', '')
        stats['def_manual'] += 1
        return text

    groups = dict(def_from_ec(row.get('translation')))
    if not groups:
        return None
    parts = []
    for pos in sorted(groups, key=lambda k: (POS_KEY.get(k, 99), k)):
        items = groups[pos]
        if not items:
            continue
        seg = '；'.join(items)
        parts.append((pos + ' ' + seg) if pos else seg)
    text = '；'.join(parts)
    text = re.sub(r'；\s*；+', '；', text).strip('；')
    stats['def_ec'] += 1
    return text or None


# ===================== flags / 词形 =====================

def build_flags(row):
    """tag/collins/oxford → 紧凑徽章码（顺序固定，便于肉眼比对）。"""
    tag = (row.get('tag') or '').lower().split()
    out = []
    for t, code in TAG_CODE.items():
        if t in tag:
            out.append(code)
    try:
        collins = int(row.get('collins') or 0)
    except ValueError:
        collins = 0
    if collins > 0:
        out.append('C%d' % min(collins, 5))
    try:
        if int(row.get('oxford') or 0) > 0:
            out.append('O')
    except ValueError:
        pass
    return ''.join(out)


def parse_exchange(s):
    """exchange 字段 → {前缀: [形式]}；前缀 0=原形 p=过去式 i=现在分词
    d=过去分词 3=三单 s=复数 r=比较级 t=最高级。"""
    out = {}
    for part in (s or '').split('/'):
        if ':' not in part:
            continue
        k, v = part.split(':', 1)
        k = k.strip()
        forms = [x.strip().lower() for x in v.split(',') if x.strip()]
        if k and forms:
            out[k] = forms
    return out


# ===================== 主流程 =====================

def main():
    ap = argparse.ArgumentParser(description="生成 dict/dict.dat + dict/dict.idx")
    ap.add_argument('--limit', type=int, default=0, help='只取前 N 条（调试）')
    ap.add_argument('--only-data', action='store_true', help='只出数据，不补字/不拷贝')
    ap.add_argument('--update-hanzi', action='store_true', help='把缺字写回 font/hanzi.txt')
    ap.add_argument('--no-copy', action='store_true', help='不写 sim/sd/dict')
    ap.add_argument('--no-zh', action='store_true', help='不合并 CC-CEDICT（只出英→中）')
    args = ap.parse_args()

    print('[1/6] 读 ecdict 并选词 …')
    freq_top = load_freq_top()
    print('      常用词表（短语首词判据）：%d 个' % len(freq_top))
    rows = read_ecdict(freq_top, args.limit or None)
    print('      选中 %d 条' % len(rows))

    print('[2/6] 拼释义 / flags / 词形 …')
    entries = {}          # word -> dict(phon, def, flags, lemma)
    for w, row in rows.items():
        d = build_definition(w, row)
        if not d:
            stats['def_missing'] += 1
            continue

        phon = clean_phon_ec(row.get('phonetic'))
        if phon and not phon_ok(phon):
            stats['phon_bad'] += 1
            phon = ''
        if not phon:
            stats['phon_empty'] += 1

        d, cut = cut_bytes(d, DEF_MAX_BYTES)
        if cut:
            long_defs.append(w)

        entries[w] = {'phon': phon, 'def': d, 'flags': build_flags(row), 'lemma': ''}

    # 词形：exchange 回填 lemma + 表外变形词补行
    base_rows = dict(entries)
    added = 0
    for w, row in rows.items():
        if w not in entries:
            continue
        ex = parse_exchange(row.get('exchange'))
        if not ex:
            continue
        base = (ex.get('0') or [w])[0]
        if base != w and base in entries:
            entries[w]['lemma'] = base
        if base not in entries:
            base = w                     # 原形不在表内就不敢指过去
        for prefix in ('p', 'i', 'd', '3', 's', 'r', 't'):
            for form in ex.get(prefix, []):
                if form == w or not WORD_RE.match(form) or form in base_rows:
                    continue
                if form in entries:
                    if entries[form]['lemma'] == '':
                        entries[form]['lemma'] = base
                    continue
                src = entries.get(base)
                if src is None:
                    continue
                entries[form] = {'phon': src['phon'], 'def': src['def'],
                                 'flags': '', 'lemma': base}
                added += 1
    stats['variant_added'] = added
    stats['lemma_filled'] = sum(1 for e in entries.values() if e['lemma'])

    print('[2b/6] 合并 CC-CEDICT 中→英词条 …')
    zh_words = []
    if not args.no_zh:
        zh = ZD.load(stats)
        added_zh = collide = 0
        for w, e in zh.items():
            if w in entries:
                collide += 1          # 极少数带 ASCII 的条目（已过滤）不会到这
                continue
            entries[w] = e
            zh_words.append(w)
            added_zh += 1
        stats['zh_added'] = added_zh
        stats['zh_collide'] = collide
        print('      中→英新增 %d 条，重名跳过 %d 条' % (added_zh, collide))
    else:
        print('      --no-zh：跳过')

    print('[2c/6] 中文字形可用性过滤（词头 / 繁体）…')
    cmap = load_cmap()
    dz = dl = 0
    for w in zh_words:
        e = entries.get(w)
        if e is None:
            continue
        if cmap is not None:
            if any(ord(c) > 127 and ord(c) not in cmap for c in w):
                del entries[w]                 # 词头有字出不来方框，整条不要
                dz += 1
                continue
            if e['lemma'] and any(ord(c) not in cmap for c in e['lemma']):
                e['lemma'] = ''                # 繁体显示不了就只留简体
                dl += 1
        else:
            # 没有 fontTools：拿 hanzi.txt 当代理。偏严（字库里有、但当前字体
            # 没烘进去的字也会被挡掉），可另一边绝不会出现方框
            if any(c not in HANZI_ALL for c in w):
                del entries[w]
                dz += 1
                continue
            if e['lemma'] and any(c not in HANZI_ALL for c in e['lemma']):
                e['lemma'] = ''
                dl += 1
    stats['zh_word_dropped'] = dz
    stats['zh_lemma_dropped'] = dl
    print('      词头没字形丢 %d 条；繁体没字形清空 %d 条' % (dz, dl))

    print('[3/6] 写主表 dict.dat …')
    lines = []
    for w in sorted(entries, key=lambda s: s.encode('utf-8')):
        e = entries[w]
        lines.append('%s\t%s\t%s\t%s\t%s' % (w, e['phon'], e['def'], e['flags'], e['lemma']))

    os.makedirs(os.path.dirname(OUT_DAT), exist_ok=True)
    offsets = []
    blob = bytearray()
    for i, ln in enumerate(lines):
        if i % STEP == 0:
            word = ln.split('\t', 1)[0]
            offsets.append((len(blob), word.encode('utf-8')[:KEY_BYTES]))
        blob += ln.encode('utf-8') + b'\n'
    with open(OUT_DAT, 'wb') as f:
        f.write(blob)

    print('[4/6] 写索引 dict.idx（%d 个锚点）…' % len(offsets))
    with open(OUT_IDX, 'wb') as f:
        f.write(struct.pack('<4sHHII8s', b'DCTX', 1, STEP, len(lines), len(blob), b'\0' * 8))
        for off, key in offsets:
            f.write(struct.pack('<I', off))
            f.write(key + b'\0' * (KEY_BYTES - len(key)))

    # 缺字统计（词头 / 繁体 / 释义里用到、但 hanzi.txt 没有的字）
    for ln in lines:
        f = ln.split('\t')
        for ch in f[0] + f[2] + f[4]:
            if ord(ch) > 127:
                used_chars.add(ch)
                if ch not in HANZI_ALL:
                    oob_chars.add(ch)

    print('[5/6] 字体覆盖检查 …')
    drop = drop_chars()
    if drop:
        print('      源字体没有字形，已从释义里丢弃：%s' % ' '.join(sorted(drop)))
        for i, ln in enumerate(lines):
            w, ph, d, fl, lm = ln.split('\t')
            d2 = ''.join(ch for ch in d if ch not in drop)
            lines[i] = '\t'.join([w, ph, d2, fl, lm])
        blob = bytearray()
        offsets = []
        for i, ln in enumerate(lines):
            if i % STEP == 0:
                word = ln.split('\t', 1)[0]
                offsets.append((len(blob), word.encode('utf-8')[:KEY_BYTES]))
            blob += ln.encode('utf-8') + b'\n'
        with open(OUT_DAT, 'wb') as f:
            f.write(blob)
        with open(OUT_IDX, 'wb') as f:
            f.write(struct.pack('<4sHHII8s', b'DCTX', 1, STEP, len(lines), len(blob), b'\0' * 8))
            for off, key in offsets:
                f.write(struct.pack('<I', off))
                f.write(key + b'\0' * (KEY_BYTES - len(key)))

    if args.update_hanzi:
        new_chars = sorted(oob_chars - drop)
        if new_chars:
            with io.open(HANZI_TXT, 'a', encoding='utf-8') as f:
                f.write('\n' + ''.join(new_chars) + '\n')
            print('      已把 %d 个缺字写回 font/hanzi.txt' % len(new_chars))
        else:
            print('      没有缺字，hanzi.txt 未改')

    print('[6/6] 报告 / 拷贝 …')
    write_report(lines, blob)
    if not args.only_data and not args.no_copy:
        copy_to_sim()

    print('完成：%d 条，主表 %.1f MB，索引 %d KB，缺字 %d 个（丢 %d 个）'
          % (len(lines), len(blob) / 1048576.0,
             (24 + 12 * len(offsets)) / 1024.0, len(oob_chars), len(drop)))


def copy_to_sim():
    """产物同步到模拟器的 SD 目录；旧 dict.csv 改名 .bak（设备端只认 .dat）。"""
    os.makedirs(SIM_DICT_DIR, exist_ok=True)
    for src, dst in ((OUT_DAT, os.path.join(SIM_DICT_DIR, 'dict.dat')),
                     (OUT_IDX, os.path.join(SIM_DICT_DIR, 'dict.idx'))):
        shutil.copy2(src, dst)
        print('      -> %s' % os.path.relpath(dst, ROOT))
    for old in OLD_CSVS:
        if os.path.exists(old):
            bak = old + '.bak'
            if not os.path.exists(bak):
                os.replace(old, bak)
                print('      旧文件改名为 %s' % os.path.relpath(bak, ROOT))


def load_cmap():
    """源字体的 cmap（码点 → 字形名）；没有 fontTools 就返回 None。

    中文字词要按「当前字体到底有没有这个字形」判断 —— font/hanzi.txt 只是**要烘进
    字体**的字符表，一开始并不包含繁体写法，光看它会把能显示的繁体也当成缺字。
    """
    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        print('      （没装 fontTools，中文字形改用 font/hanzi.txt 判定，偏严）')
        return None
    if not os.path.exists(TTF):
        return None
    return TTFont(TTF, lazy=True).getBestCmap()


def drop_chars():
    """用 fontTools 查源字体 cmap：返回「源字体里没有字形」的字符集合。"""
    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        print('      （没装 fontTools，跳过字体覆盖检查）')
        return set()
    if not os.path.exists(TTF):
        return set()
    cmap = TTFont(TTF, lazy=True).getBestCmap()
    miss = {ch for ch in (used_chars | oob_chars) if ord(ch) not in cmap}
    no_glyph_chars.update(miss)
    return miss


def write_report(lines, blob):
    flags = Counter()
    for ln in lines:
        for ch in ln.split('\t')[3]:
            flags[ch if ch != 'C' else 'C*'] += 1
    maxb = max(len(ln.encode('utf-8')) for ln in lines)
    out = []
    out.append('lines: %d' % len(lines))
    out.append('bytes: %d (%.2f MB)' % (len(blob), len(blob) / 1048576.0))
    out.append('max line bytes: %d' % maxb)
    out.append('anchors: %d (step=%d)' % ((len(lines) + STEP - 1) // STEP, STEP))
    out.append('stats: %s' % dict(stats))
    out.append('flags 分布: %s' % dict(flags.most_common()))
    out.append('截断释义: %d 条 %s' % (len(long_defs), long_defs[:20]))
    out.append('字库外汉字: %d 个 %s' % (len(oob_chars), ''.join(sorted(oob_chars))))
    out.append('源字体缺字形被丢: %d 个 %s'
               % (len(no_glyph_chars), ''.join(sorted(no_glyph_chars))))
    io.open(REPORT, 'w', encoding='utf-8').write('\n'.join(out) + '\n')
    print('      报告 -> tools/build_report.txt')


HANZI_ALL = frozenset(io.open(HANZI_TXT, encoding='utf-8').read())


if __name__ == '__main__':
    main()