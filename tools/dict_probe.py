# -*- coding: utf-8 -*-
"""设备端查找算法的 Python 复刻 —— 直接在 PC 上验证词库能不能查出来。

它按 `main/services/srv_dict.c` 的实现步骤逐条复刻：
  1. 锚点表二分（块首词前 8 字节）→ 定位到某个 64 行的块
  2. 顺序扫该块（最多再往后多扫一块），命中 / 越过即停
  3. 词形还原：先精确，再按规则去 -s/-ed/-ing/… 与辅音双写、-ied→y 重查
  4. 前缀联想 / 拼写容错（同前两字母的桶 + 编辑距离 ≤1，读到 64KB 就放弃）
  5. 逐词直译：按 [A-Za-z][A-Za-z'-]* 切 token，逐词走词形还原

所以**探针的结果就是真机上的预期结果**，改 C 代码前先用它把词库钉死。

用法：
    python tools/dict_probe.py                 # 跑内置代表用例
    python tools/dict_probe.py --word running  # 单查一个词
    python tools/dict_probe.py --sentence "hello world"
"""

import argparse
import io
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DAT = os.path.join(ROOT, 'dict', 'dict.dat')
IDX = os.path.join(ROOT, 'dict', 'dict.idx')

SUGGEST_MAX_BYTES = 64 * 1024
BLOCK_EXTRA_LINES = 128         # 跨块兜底：最多连读两块


def stem_candidates(w):
    """规则词形还原的候选（按优先级）：与 srv_dict.c 里的实现保持一致。"""
    out = []
    if len(w) < 3:
        return out

    def add(base):
        if base and base != w and base not in out:
            out.append(base)

    # -ies / -ied → y
    if w.endswith('ies') or w.endswith('ied'):
        add(w[:-3] + 'y')
    # -es / -s
    if w.endswith('es'):
        add(w[:-2])
        add(w[:-1])
    elif w.endswith('s') and not w.endswith('ss'):
        add(w[:-1])
    # -ed / -ing
    if w.endswith('ed'):
        add(w[:-2])
        add(w[:-1])
    if w.endswith('ing'):
        add(w[:-3])
        add(w[:-3] + 'e')
    # -ly / -er / -est
    if w.endswith('ly'):
        add(w[:-2])
    if w.endswith('er'):
        add(w[:-2])
        add(w[:-1])
    if w.endswith('est'):
        add(w[:-3])
        add(w[:-2])
    # 辅音双写：running→runn→run、stopped→stopp→stop
    # 要多轮收缩：`runnning`（三个 n）得 runnn → runn → run 才落到位
    for _ in range(3):
        grew = False
        for base in list(out):
            if len(base) > 2 and base[-1] == base[-2] and base[:-1] not in out:
                out.append(base[:-1])
                grew = True
        if not grew:
            break
    # sunny→sunn→sun 这类 y→i 的（studies 已在上面处理）
    for base in list(out):
        if base.endswith('i'):
            add(base[:-1] + 'y')
    return out


def edit_distance_le1(a, b):
    """a 与 b 的编辑距离是否 ≤1（含换位/增删/改）。"""
    if a == b:
        return True
    la, lb = len(a), len(b)
    if abs(la - lb) > 1:
        return False
    if la == lb:
        diff = [i for i in range(la) if a[i] != b[i]]
        if len(diff) == 1:
            return True
        # 相邻换位
        if len(diff) == 2 and diff[1] == diff[0] + 1:
            i = diff[0]
            return a[i] == b[i + 1] and a[i + 1] == b[i]
        return False
    if la > lb:
        a, b, la, lb = b, a, lb, la
    i = j = 0
    skipped = False
    while i < la and j < lb:
        if a[i] != b[j]:
            if skipped:
                return False
            skipped = True
            j += 1
            continue
        i += 1
        j += 1
    return True


class Dict(object):
    def __init__(self, dat=DAT, idx=IDX):
        self.f = io.open(dat, 'rb')
        raw = io.open(idx, 'rb').read()
        magic, ver, self.step, self.count, self.size, _ = struct.unpack('<4sHHII8s', raw[:24])
        assert magic == b'DCTX', '索引 magic 不对'
        self.anchors = []
        for i in range((len(raw) - 24) // 12):
            o, key = struct.unpack('<I8s', raw[24 + i * 12:24 + i * 12 + 12])
            self.anchors.append((o, key))

    # ---- 底层 ----
    def _anchor_of(self, word):
        """最后一个 key <= word 前 8 字节的锚点（等价于 C 里的 upper_bound-1）。"""
        tk = (word.encode('utf-8') + b'\0' * 8)[:8]
        lo, hi = 0, len(self.anchors)
        while lo < hi:
            mid = (lo + hi) // 2
            if self.anchors[mid][1] <= tk:
                lo = mid + 1
            else:
                hi = mid
        return max(0, lo - 1)

    def _read_from(self, ai, want_prefix, max_lines):
        """从第 ai 块开始顺序读；返回 [(word, line)]，读到越过前缀就停。"""
        self.f.seek(self.anchors[ai][0])
        out = []
        for _ in range(max_lines):
            raw = self.f.readline()
            if not raw:
                break
            line = raw.decode('utf-8').rstrip('\n')
            w = line.split('\t', 1)[0]
            out.append((w, line))
            if not w.startswith(want_prefix) and w.encode('utf-8') > want_prefix.encode('utf-8'):
                break
        return out

    # ---- 对外 ----
    def exact(self, word):
        w = word.strip().lower()
        ai = self._anchor_of(w)
        for ww, line in self._read_from(ai, w, BLOCK_EXTRA_LINES):
            if ww == w:
                return self._parse(line)
            if ww.encode('utf-8') > w.encode('utf-8'):
                break
        return None

    def lookup_lemma(self, word):
        """返回 (entry, 原形提示)。原形提示 = 该词对应的原形（可空）。"""
        w = word.strip().lower()
        e = self.exact(w)
        if e is not None:
            return e, (e['lemma'] or None)
        for cand in stem_candidates(w):
            e = self.exact(cand)
            if e is not None:
                return e, e['word']
        return None, None

    def prefix(self, p, maxn=40):
        p = p.strip().lower()
        ai = self._anchor_of(p)
        out = []
        for ww, line in self._read_from(ai, p, 4096):
            if ww.startswith(p):
                out.append(self._parse(line))
                if len(out) >= maxn:
                    break
        return out

    def suggest(self, word, maxn=3):
        """拼写容错：同前两字母 + 编辑距离 ≤1（硬上限 64KB 扫描）。

        排序与 srv_dict.c 一致：有考纲/牛津/Collins 徽章的常用词优先，
        其次长度更接近，最后字母序。"""
        w = word.strip().lower()
        if len(w) < 3:
            return []
        pair = w[:2]
        cands = []
        read_bytes = 0
        si = None
        for i, (o, key) in enumerate(self.anchors):
            if key[:2] == pair.encode('ascii'):
                si = i
                break
        if si is None:
            return []
        for i in range(si, len(self.anchors)):
            if self.anchors[i][1][:2] != pair.encode('ascii'):
                break
            end = self.anchors[i + 1][0] if i + 1 < len(self.anchors) else self.size
            read_bytes += max(0, end - self.anchors[i][0])
            for ww, line in self._read_from(i, pair, self.step + 1):
                if abs(len(ww) - len(w)) <= 2 and edit_distance_le1(ww, w):
                    cands.append(self._parse(line))
            if read_bytes > SUGGEST_MAX_BYTES:
                break

        def flags_score(flags):
            """与 C 的 sug_score 一致：标签个数 + Collins 星数。"""
            n = 0
            i = 0
            while i < len(flags):
                if flags[i] == 'C' and i + 1 < len(flags) and flags[i + 1].isdigit():
                    n += 1 + int(flags[i + 1])
                    i += 2
                else:
                    n += 1
                    i += 1
            return n

        def key(e):
            # 常用度高的优先，其次长度更接近，最后字母序
            return (-flags_score(e['flags']), abs(len(e['word']) - len(w)), e['word'])

        cands.sort(key=key)
        return cands[:maxn]

    def translate(self, text):
        """逐词直译：[(token, entry|None)]，token 保持原文大小写。"""
        out = []
        for tok in re.findall(r"[A-Za-z][A-Za-z'\-]*", text):
            e, _ = self.lookup_lemma(tok)
            out.append((tok, e))
        return out

    @staticmethod
    def _parse(line):
        w, phon, dfn, flags, lemma = line.split('\t')
        return {'word': w, 'phon': phon, 'def': dfn, 'flags': flags, 'lemma': lemma}


CASES = [
    ('running', '精确命中 + lemma=run'),
    ('went', '规则/表内还原到 go'),
    ('runs', 'lemma=run'),
    ('give up', '短语'),
    ('Apple', '大小写不敏感'),
    ('Runnning', '拼写错 + 词形还原（run / running）'),
    ('zzzz', '未命中且无建议'),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--word')
    ap.add_argument('--sentence')
    args = ap.parse_args()
    d = Dict()

    if args.word:
        e, lemma = d.lookup_lemma(args.word)
        print('lookup_lemma(%r):' % args.word)
        print('   entry : %s' % ('-' if not e else ' | '.join(
            [e['word'], e['phon'] or '-', e['def'], e['flags'] or '-', e['lemma'] or '-'])))
        print('   提示  : %s' % (lemma or '-'))
        print('   建议  : %s' % ', '.join(x['word'] for x in d.suggest(args.word)))
        print('   联想  : %s' % ', '.join(x['word'] for x in d.prefix(args.word, 8)))
        return

    if args.sentence:
        for tok, e in d.translate(args.sentence):
            print('%-12s %s' % (tok, (e['def'].split('；')[0] if e else '—')))
        return

    ok = True
    print('== 代表用例 ==')
    for word, note in CASES:
        e, lemma = d.lookup_lemma(word)
        sug = [x['word'] for x in d.suggest(word)]
        print('%-12s %-38s -> %s%s'
              % (word, note,
                 (e['word'] + (' (lemma=%s)' % e['lemma'] if e['lemma'] else '')) if e else '未命中',
                 ('  建议=' + ','.join(sug)) if sug else ''))
        if word == 'running' and (not e or e['word'] != 'running' or e['lemma'] != 'run'):
            ok = False
        if word == 'went' and (not e or e['lemma'] != 'go'):
            ok = False
        if word == 'give up' and not e:
            ok = False
        if word == 'Apple' and (not e or e['word'] != 'apple'):
            ok = False
        if word == 'Runnning' and (not e or e['word'] not in ('run', 'running')):
            ok = False
        if word == 'zzzz' and (e or sug):
            ok = False

    print('== 直译 ==')
    for tok, e in d.translate('Hello world, I love this book'):
        print('   %-10s %s' % (tok, (e['def'].split('；')[0] if e else '—')))

    print('== 前缀联想 apple ==')
    print('   ' + ', '.join(x['word'] for x in d.prefix('apple', 10)))

    print('\n结果：' + ('全部通过' if ok else '有用例不符合预期'))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()