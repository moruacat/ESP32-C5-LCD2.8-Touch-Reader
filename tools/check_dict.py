# -*- coding: utf-8 -*-
"""校验生成出来的词库：`dict/dict.dat` + `dict/dict.idx`。

检查项（生成器的产出必须全部通过，设备端才敢直接用）：
  dict.dat  编码/行尾、每行恰好 4 个 TAB、词形合法、**字节序严格递增**、无重复、
            音标字符白名单、释义汉字都在 font/hanzi.txt 内、行长与释义长度上限、
            lemma 必须指向表内真实存在的词
  dict.idx  magic/版本/步长/条数/文件大小、锚点偏移递增且落在行首、
            锚点 key == 该块首词的前 8 字节

用法：
    python tools/check_dict.py [--dat dict/dict.dat] [--idx dict/dict.idx] [--quiet]
"""

import argparse
import io
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_dict import HANZI_TXT, PHON_IPA, WORD_RE, DEF_MAX_BYTES   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LINE_MAX = 640
KEY_BYTES = 8


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dat', default=os.path.join(ROOT, 'dict', 'dict.dat'))
    ap.add_argument('--idx', default=os.path.join(ROOT, 'dict', 'dict.idx'))
    ap.add_argument('--quiet', action='store_true')
    args = ap.parse_args()

    problems = []
    hanzi = frozenset(io.open(HANZI_TXT, encoding='utf-8').read())

    raw = io.open(args.dat, 'rb').read()
    if raw.startswith(b'\xef\xbb\xbf'):
        problems.append('dict.dat 带 UTF-8 BOM')
    if b'\r' in raw:
        problems.append('dict.dat 里有 CR（必须是纯 LF）')

    words = []
    lines = raw.split(b'\n')
    if lines and lines[-1] == b'':
        lines.pop()
    max_line = 0
    for i, ln in enumerate(lines):
        try:
            text = ln.decode('utf-8')
        except UnicodeDecodeError as e:
            problems.append('第 %d 行不是合法 UTF-8：%s' % (i + 1, e))
            continue
        max_line = max(max_line, len(ln))
        parts = text.split('\t')
        if len(parts) != 5:
            problems.append('第 %d 行不是 5 字段（TAB 数 %d）：%r' % (i + 1, len(parts) - 1, text[:40]))
            continue
        w, phon, dfn, flags, lemma = parts
        if not WORD_RE.match(w):
            problems.append('第 %d 行词形非法：%r' % (i + 1, w))
        if not dfn:
            problems.append('第 %d 行释义为空：%r' % (i + 1, w))
        for ch in phon:
            if ord(ch) > 127 and ch not in PHON_IPA:
                problems.append('第 %d 行音标越界字符 %r：%s' % (i + 1, ch, w))
                break
        if len(dfn.encode('utf-8')) > DEF_MAX_BYTES + 4:
            problems.append('第 %d 行释义超长 %d 字节：%s'
                            % (i + 1, len(dfn.encode('utf-8')), w))
        for ch in dfn:
            if ord(ch) > 127 and ch not in hanzi and ch != '…':
                problems.append('第 %d 行释义里有字库外字符 %r：%s' % (i + 1, ch, w))
                break
        if len(flags) > 16:
            problems.append('第 %d 行 flags 异常长：%s' % (i + 1, flags))
        words.append(w)

    # 排序 / 去重
    enc = [w.encode('utf-8') for w in words]
    for i in range(1, len(enc)):
        if enc[i] <= enc[i - 1]:
            problems.append('排序错误/重复：第 %d 行 %r 不大于上一行 %r'
                            % (i + 1, words[i], words[i - 1]))
            if len(problems) > 40:
                break

    # lemma 悬空
    wset = set(words)
    dangling = 0
    for ln in lines:
        parts = ln.decode('utf-8', 'ignore').split('\t')
        if len(parts) == 5 and parts[4] and parts[4] not in wset:
            dangling += 1
    if dangling:
        problems.append('有 %d 条 lemma 指向表里不存在的词' % dangling)

    # 索引
    idx = io.open(args.idx, 'rb').read()
    if len(idx) < 24:
        problems.append('dict.idx 太短')
    else:
        magic, ver, step, count, size, _ = struct.unpack('<4sHHII8s', idx[:24])
        if magic != b'DCTX':
            problems.append('dict.idx magic 不对：%r' % magic)
        if count != len(words):
            problems.append('dict.idx 条数 %d != dict.dat 行数 %d' % (count, len(words)))
        if size != len(raw):
            problems.append('dict.idx 记录的主表大小 %d != 实际 %d' % (size, len(raw)))
        if ver != 1:
            problems.append('dict.idx 版本不是 1：%d' % ver)
        anchors = (len(idx) - 24) // 12
        want = (len(words) + step - 1) // step
        if anchors != want:
            problems.append('锚点数 %d != 期望 %d' % (anchors, want))
        # 偏移必须落在行首，且 key == 该块首词前 8 字节
        line_start = set()
        off = 0
        for ln in lines:
            line_start.add(off)
            off += len(ln) + 1
        prev_off = -1
        for i in range(anchors):
            o, key = struct.unpack('<I8s', idx[24 + i * 12:24 + i * 12 + 12])
            if o not in line_start:
                problems.append('锚点 %d 的偏移 %d 不在行首' % (i, o))
            if o <= prev_off:
                problems.append('锚点 %d 的偏移没有递增' % i)
            prev_off = o
            wi = i * step
            if wi < len(words):
                expect = words[wi].encode('utf-8')[:KEY_BYTES]
                if key.rstrip(b'\0') != expect:
                    problems.append('锚点 %d 的 key %r != 块首词前缀 %r'
                                    % (i, key.rstrip(b'\0'), expect))

    # 结果
    if not args.quiet:
        print('dict.dat : %d 条，%d 字节（%.2f MB），最长行 %d 字节'
              % (len(words), len(raw), len(raw) / 1048576.0, max_line))
        print('dict.idx : %d 字节' % len(idx))
    if problems:
        print('!! 发现 %d 个问题：' % len(problems))
        for p in problems[:60]:
            print('   - ' + p)
        sys.exit(1)
    print('OK：词库与索引一致，可以用')


if __name__ == '__main__':
    main()