#!/usr/bin/env python3
"""扫描电子书（.ebl / .epub），找出字体字符集里没有的字符。

用法：
    python tools/check_charset.py "D:\\path\\book.eb"          # EBF 目录（含 chapter.*.ebl）
    python tools/check_charset.py "D:\\path\\chapter.1.正文.ebl"   # 单个 EBF 章节
    python tools/check_charset.py "D:\\path\\a.epub" "D:\\path\\b.epub"
    python tools/check_charset.py "D:\\path\\novels"           # 目录下所有 .ebl / .epub

字符集取 main/assets/fonts/charset.hex —— 那是 gen_font.py 最终交给
lv_font_conv 的码点表，也就是 font_ui_14 里真实存在的字形。

LVGL 找不到字形时 adv_w 为 0、字符直接不绘制（不是豆腐块），表现为
「整字消失」，肉眼不容易发现，所以导入新书前值得跑一遍。

退出码：0 = 全部支持；2 = 有缺字；1 = 用法/路径错误。
"""

import os
import sys
import zipfile
from collections import Counter
from html.parser import HTMLParser

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHARSET = os.path.join(ROOT, "main", "assets", "fonts", "charset.hex")
BODY_TAG = "<body>"
EPUB_TEXT_EXT = (".xhtml", ".html", ".htm")
BOOK_EXT = (".ebl", ".epub")
# 这些码点本来就不可见 / 只影响排版，不算「缺字」，单独归一类
INVISIBLE = {
    0x00A0,  # 不换行空格
    0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005, 0x2006, 0x2007, 0x2008, 0x2009, 0x200A,
    0x200B, 0x200C, 0x200D, 0x200E, 0x200F,  # 零宽 / 方向标记
    0x2028, 0x2029,  # 行/段分隔符
    0x202F, 0x205F, 0x2060, 0x3000,
    0xFEFF,  # BOM
}


def load_charset(path):
    cps = set()
    with open(path, encoding="ascii") as f:
        for line in f:
            line = line.strip()
            if line:
                cps.add(int(line, 16))
    return cps


def read_ebl(path):
    """读 .ebl 并截掉 <body> 之前的元数据。"""
    with open(path, encoding="utf-8", errors="replace") as f:
        raw = f.read()
    i = raw.find(BODY_TAG)
    return raw[i + len(BODY_TAG):] if i >= 0 else raw


class _TextOnly(HTMLParser):
    """从 XHTML 里抽可见文本，跳过 script/style。"""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self._skip = 0
        self._buf = []

    def handle_starttag(self, tag, attrs):
        if tag in ("script", "style"):
            self._skip += 1

    def handle_endtag(self, tag):
        if tag in ("script", "style") and self._skip:
            self._skip -= 1

    def handle_data(self, data):
        if not self._skip:
            self._buf.append(data)

    def text(self):
        return "\n".join(self._buf)


def read_epub(path):
    """把 EPUB 里所有 XHTML 的可见文本拼起来。"""
    parts = []
    try:
        with zipfile.ZipFile(path) as z:
            for name in z.namelist():
                if not name.lower().endswith(EPUB_TEXT_EXT):
                    continue
                try:
                    raw = z.read(name)
                except (KeyError, zipfile.BadZipFile, OSError):
                    continue
                parser = _TextOnly()
                try:
                    parser.feed(raw.decode("utf-8", errors="replace"))
                except Exception:       # 个别书里有坏标记，跳过这一篇就够
                    continue
                t = parser.text()
                if t:
                    parts.append(t)
    except (zipfile.BadZipFile, OSError) as e:
        print("打开失败 %s: %s" % (os.path.basename(path), e))
    return "\n".join(parts)


def read_text(path):
    if path.lower().endswith(".epub"):
        return read_epub(path)
    return read_ebl(path)


def collect_files(targets):
    files = []
    for t in targets:
        if os.path.isfile(t):
            files.append(t)
        elif os.path.isdir(t):
            for n in sorted(os.listdir(t)):
                if n.lower().endswith(BOOK_EXT):
                    files.append(os.path.join(t, n))
        else:
            print("跳过（不存在）: %s" % t)
    return files


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    files = collect_files(sys.argv[1:])
    if not files:
        print("没有找到可扫描的文件（.ebl / .epub）")
        return 1

    charset = load_charset(CHARSET)
    print("字符集: %s（%d 个码点）" % (os.path.relpath(CHARSET, ROOT), len(charset)))
    print("待扫描: %d 个文件" % len(files))

    hits = Counter()          # 可见缺字 -> 出现次数
    invis = Counter()         # 不可见/排版字符 -> 出现次数
    first_ctx = {}            # 字符 -> (书名, 上下文)
    total_chars = 0
    bad_files = 0

    for path in files:
        book = os.path.basename(path)
        try:
            text = read_text(path)
        except OSError:
            bad_files += 1
            continue
        if not text:
            bad_files += 1
            continue
        total_chars += len(text)
        for idx, ch in enumerate(text):
            cp = ord(ch)
            if cp < 0x20:           # 换行/缩进等控制字符不算缺字
                continue
            if cp in charset:
                continue
            if cp in INVISIBLE:
                invis[ch] += 1
                continue
            hits[ch] += 1
            if ch not in first_ctx:
                s = max(0, idx - 12)
                e = min(len(text), idx + 13)
                ctx = text[s:e].replace("\r", " ").replace("\n", " ")
                first_ctx[ch] = (book, ctx)

    print("合计 %d 个字符，%d 个文件读取失败/为空" % (total_chars, bad_files))

    if invis:
        print("\n另有 %d 个不可见/排版字符（不影响阅读，不用补）："
              % len(invis))
        print("  " + " ".join("U+%04X×%d" % (ord(c), n) for c, n in invis.most_common()))

    if not hits:
        print("\n结论：全部可见字符都在字符集内。")
        return 0

    print("\n结论：%d 个可见字符不在字符集内（按出现次数排序）：" % len(hits))
    print("%-4s %-8s %-6s  %s" % ("字符", "码点", "次数", "首次出现"))
    for ch, n in hits.most_common():
        book, ctx = first_ctx[ch]
        print("%-4s U+%04X  %-6d  %s | %s" % (ch, ord(ch), n, book, ctx))
    return 2


if __name__ == "__main__":
    sys.exit(main())
