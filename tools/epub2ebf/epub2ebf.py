#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
epub2ebf —— 把 EPUB 转成本项目的 EBF 书籍文件夹

用法:
    python epub2ebf.py 书名.epub                  # 就地生成 书名.ebf/ 文件夹
    python epub2ebf.py a.epub b.epub -o E:\\books  # 批量转到指定目录
    python epub2ebf.py D:\\downloads -o E:\\books  # 目录里所有 epub 逐个转

输出结构:
    书名.ebf/
    ├── chapter.N.ebfl      XML 首部（章名/卷名）+ 空行 + 章节正文原文
    ├── chapters.ebfl       章名索引：一行一名，行序 = 章号（大纲用）
    ├── data.ebfl           XML 元数据：book@chapters / title / author / intro / cover / volumes
    └── cover.raw           封面 RGB565 小端，等比缩放到不超过 240x320

关于两份章名（首部 + 索引）：
    章节文件的首部让"只想知道这一章叫什么"不用开别的文件（固件读几百字节就够）；
    索引是给跳转大纲用的 —— 要一次列出全部章名，靠逐章开文件在 SD 卡上要几秒。
    两份都由本工具一次写死，不存在运行时不一致。

`<book chapters="N">` 是给固件用的：有了它就不用一章一章 stat 去数章节数
（几百次 SD 目录查找），读不到时才回退到倍增探测 + 二分。

关于切章：以 EPUB 自带目录(TOC)为章节边界，目录里的一级条目（带子条目）
记为分卷，叶子条目记为章节，正文按 spine 顺序归入对应章节。没有目录的
EPUB 退回按 spine 里每个 xhtml 文件切一章。

依赖: Python 3.12+；封面需要 Pillow（没装则跳过封面，其余照常）
"""

from __future__ import annotations

import argparse
import io
import posixpath
import re
import shutil
import sys
import zipfile
from dataclasses import dataclass
from html.parser import HTMLParser
from pathlib import Path
from typing import Callable
from urllib.parse import unquote
from xml.etree import ElementTree as ET

# ===================== 与固件对齐的常量（改这里要同步改 main/services） =====================

COVER_MAX_W = 240           # 封面等比缩放上限，不放大
COVER_MAX_H = 320
CHAPTER_BYTES_WARN = 4 * 1024 * 1024    # srv_reader 的单章上限（SRV_READER_TEXT_MAX）

# 章节数**不设上限**：本工具保证 chapter.N.ebfl 从 1 连续编号，并把总数写进
# data.ebfl 的 <book chapters="N">，固件直接读这个数，不去数文件。所以这里
# 既不需要卡上限，也不该合并章节（合并会把后面的章节结构毁掉）。

TITLE_MAX_BYTES = 180       # srv_library 的 SRV_BOOK_NAME_MAX = 192
AUTHOR_MAX_BYTES = 44       # SRV_BOOK_AUTHOR_MAX = 48

# 简介上限。注意这里**不是** SRV_BOOK_INTRO_MAX(192)：
# 那个字段只是「这本书有没有简介」的标志位（菜单要不要显示"查看简介"），
# 真正展示时固件是拿 srv_library_read_intro() 现场从 data.ebfl 读全文的
# （读 32KB，装进界面那个 2KB 缓冲），所以这里按 192 截就等于把简介截没了。
INTRO_MAX_BYTES = 2000      # 与界面侧 2048 字节缓冲对齐

EBF_DIR_SUFFIX = ".ebf"
CHAPTER_FMT = "chapter.{}.ebfl"

# 进度回调：(已完成, 总数, 说明文字)
ProgressCb = Callable[[int, int, str], None]


class ConvertError(Exception):
    """转换失败，消息直接给用户看"""


# ===================== 文本清洗 =====================

_WS_RE = re.compile(r"[ \t\r\n\f\v\u00a0\u3000]+")
# 中日韩文字与全角标点之间的空格是 HTML 源码折行带进来的，中文排版里不该有
_CJK = "\u2e80-\u4dbf\u4e00-\u9fff\u3000-\u303f\uff00-\uffef"
_CJK_SPACE_RE = re.compile(f"(?<=[{_CJK}]) (?=[{_CJK}])")


def clean_text(s: str, blank_line: bool = False) -> str:
    """
    把抽出来的文本整理成阅读器要的样子：一行一段、没有多余空白。

    blank_line=True 时每段之间再空一行（默认不空，小屏更省地方）。
    """
    s = s.replace("\r\n", "\n").replace("\r", "\n")

    lines: list[str] = []
    for raw in s.split("\n"):
        line = _WS_RE.sub(" ", raw).strip()
        line = _CJK_SPACE_RE.sub("", line)
        lines.append(line)

    out: list[str] = []
    blank = False
    for line in lines:
        if line:
            out.append(line)
            blank = False
        elif out and not blank:
            out.append("")
            blank = True

    body = "\n".join(l for l in out if l)
    if not body:
        return ""
    sep = "\n\n" if blank_line else "\n"
    return body.replace("\n", sep) + "\n"


# ===================== HTML → 纯文本 =====================

_SKIP_TAGS = {"script", "style", "head", "title", "svg", "math", "noscript",
              "template", "meta", "link", "canvas", "iframe", "object"}
_BLOCK_TAGS = {"p", "div", "section", "article", "aside", "header", "footer",
               "main", "nav", "blockquote", "pre", "figure", "figcaption",
               "ul", "ol", "li", "dl", "dt", "dd", "table", "thead", "tbody",
               "tfoot", "tr", "td", "th", "center", "address", "fieldset",
               "form", "details", "summary", "caption"}
_HEADING_TAGS = {"h1", "h2", "h3", "h4", "h5", "h6"}


class HtmlToText(HTMLParser):
    """
    把一章的 xhtml 抽成纯文本。

    块级标签只在边界留一个换行（标题前后留两个），段内空白压成单空格，
    所以 <div> 套 <div> 再套 <p> 也不会多出空行。
    """

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self._parts: list[str] = []
        self._skip = 0
        self._pending = 0

    def _nl(self, n: int) -> None:
        self._pending = max(self._pending, n)

    def _flush_nl(self) -> None:
        if self._pending:
            self._parts.append("\n" * self._pending)
            self._pending = 0

    def handle_starttag(self, tag, attrs):
        if tag in _SKIP_TAGS:
            self._skip += 1
            return
        if self._skip:
            return
        if tag == "br":
            self._nl(1)
        elif tag in _HEADING_TAGS or tag == "hr":
            self._nl(2)
        elif tag in _BLOCK_TAGS:
            self._nl(1)

    def handle_startendtag(self, tag, attrs):
        if tag in _SKIP_TAGS:
            return
        if tag == "br":
            self._nl(1)
        elif tag in _HEADING_TAGS or tag == "hr":
            self._nl(2)
        elif tag in _BLOCK_TAGS:
            self._nl(1)

    def handle_endtag(self, tag):
        if tag in _SKIP_TAGS:
            self._skip = max(0, self._skip - 1)
            return
        if self._skip:
            return
        if tag in _HEADING_TAGS or tag == "hr":
            self._nl(2)
        elif tag in _BLOCK_TAGS:
            self._nl(1)

    def handle_data(self, data):
        if self._skip or not data:
            return
        text = _WS_RE.sub(" ", data)
        if not text.strip():
            return
        self._flush_nl()
        self._parts.append(text)

    def result(self, blank_line: bool = False) -> str:
        self._flush_nl()
        return clean_text("".join(self._parts), blank_line)


def html_to_text(raw: bytes, blank_line: bool = False) -> str:
    p = HtmlToText()
    p.feed(decode_bytes(raw))
    p.close()
    return p.result(blank_line)


# ===================== 编码 =====================

_CHARSET_RE = re.compile(rb"""charset\s*=\s*["']?\s*([\w\-]+)""", re.I)
_XML_ENC_RE = re.compile(rb"""encoding\s*=\s*["']([\w\-]+)["']""", re.I)


def decode_bytes(raw: bytes) -> str:
    """EPUB 规定正文是 UTF-8/UTF-16；这里按 BOM → 声明 → 兜底 的顺序试"""
    if raw.startswith(b"\xef\xbb\xbf"):
        return raw[3:].decode("utf-8", "replace")
    if raw.startswith((b"\xff\xfe", b"\xfe\xff")):
        return raw.decode("utf-16", "replace")

    head = raw[:2048]
    m = _XML_ENC_RE.search(head) or _CHARSET_RE.search(head)
    if m:
        enc = m.group(1).decode("ascii", "replace")
        try:
            return raw.decode(enc, "replace")
        except LookupError:
            pass

    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        pass
    for enc in ("gbk", "big5"):
        try:
            return raw.decode(enc, "replace")
        except LookupError:
            continue
    return raw.decode("latin-1", "replace")


# ===================== 路径工具 =====================

_ILLEGAL_NAME_RE = re.compile(r'[<>:"/\\|?*\x00-\x1f]')
_RESERVED = {"CON", "PRN", "AUX", "NUL", *(f"COM{i}" for i in range(1, 10)),
             *(f"LPT{i}" for i in range(1, 10))}


def truncate_utf8(s: str, max_bytes: int) -> str:
    """按字节截断，且不会把半个汉字切出来"""
    b = s.encode("utf-8")
    if len(b) <= max_bytes:
        return s
    b = b[:max_bytes]
    while b:
        try:
            return b.decode("utf-8")
        except UnicodeDecodeError:
            b = b[:-1]
    return ""


def safe_dir_name(title: str) -> str:
    """书名 → 能当文件夹名的字符串（FATFS / Windows 都不喜欢的字符换掉）"""
    name = _ILLEGAL_NAME_RE.sub("_", title)
    name = re.sub(r"\s+", " ", name).strip().strip(".").strip()
    if name.upper() in _RESERVED:
        name += "_"
    return name[:100] or "book"


def resolve_href(base_dir: str, href: str) -> str:
    """把相对 href 解析成 zip 内的绝对路径（去 fragment、去 URL 编码）"""
    href = href.split("#", 1)[0].replace("\\", "/")
    href = unquote(href)
    if not href:
        return ""
    joined = posixpath.join(base_dir, href) if base_dir else href
    return posixpath.normpath(joined).lstrip("/")


def xml_escape(s: str) -> str:
    return (s.replace("&", "&amp;").replace("<", "&lt;")
             .replace(">", "&gt;").replace('"', "&quot;"))


# ===================== TOC 节点 =====================

class TocNode:
    __slots__ = ("title", "href", "path", "children")

    def __init__(self, title: str = "", href: str = ""):
        self.title = title
        self.href = href
        self.path = ""              # 解析后的 zip 路径，空 = 指向的文件不在书里
        self.children: list[TocNode] = []


class _NavParser(HTMLParser):
    """解析 EPUB3 的 nav.xhtml，只关心 <nav> 里的 ol/li/a 层级"""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.navs: list[tuple[dict, list[TocNode]]] = []
        self._nav_depth = 0
        self._cur: tuple[dict, list[TocNode]] | None = None
        self._ol_stack: list[list[TocNode]] = []
        self._li_stack: list[TocNode] = []
        self._owner: TocNode | None = None

    def handle_starttag(self, tag, attrs):
        if tag == "nav":
            self._nav_depth += 1
            if self._nav_depth == 1:
                self._cur = ({k.lower(): v for k, v in attrs}, [])
                self.navs.append(self._cur)
            return
        if self._nav_depth != 1 or self._cur is None:
            return

        if tag == "ol":
            if self._li_stack:
                container = self._li_stack[-1].children
            else:
                container = self._cur[1]
            self._ol_stack.append(container)
            self._owner = None
        elif tag == "li":
            node = TocNode()
            container = self._ol_stack[-1] if self._ol_stack else self._cur[1]
            container.append(node)
            self._li_stack.append(node)
            self._owner = node
        elif tag == "a" and self._li_stack:
            href = dict(attrs).get("href")
            if href:
                self._li_stack[-1].href = href

    def handle_endtag(self, tag):
        if tag == "nav":
            self._nav_depth = max(0, self._nav_depth - 1)
            if self._nav_depth == 0:
                self._cur = None
                self._ol_stack.clear()
                self._li_stack.clear()
                self._owner = None
            return
        if self._nav_depth != 1:
            return
        if tag == "ol":
            if self._ol_stack:
                self._ol_stack.pop()
            self._owner = self._li_stack[-1] if self._li_stack else None
        elif tag == "li":
            if self._li_stack:
                self._li_stack.pop()
            self._owner = self._li_stack[-1] if self._li_stack else None

    def handle_data(self, data):
        if self._nav_depth == 1 and self._owner is not None:
            self._owner.title += data


def _local(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def parse_ncx(raw: bytes) -> list[TocNode]:
    """EPUB2 的 toc.ncx"""
    try:
        root = ET.fromstring(raw)
    except ET.ParseError:
        return []

    def points(parent) -> list[TocNode]:
        out: list[TocNode] = []
        for el in parent:
            if _local(el.tag) != "navPoint":
                continue
            node = TocNode()
            subs = []
            for ch in el:
                name = _local(ch.tag)
                if name == "navLabel":
                    for t in ch.iter():
                        if _local(t.tag) == "text":
                            node.title += t.text or ""
                elif name == "content":
                    node.href = ch.get("src", "")
                elif name == "navPoint":
                    subs.append(ch)
            node.children = points(subs)
            out.append(node)
        return out

    for el in root.iter():
        if _local(el.tag) == "navMap":
            return points(el)
    return []


# ===================== EPUB =====================

class EpubBook:
    def __init__(self, path: Path):
        self.path = path
        try:
            self.zf = zipfile.ZipFile(path)
        except (zipfile.BadZipFile, OSError) as e:
            raise ConvertError(f"不是有效的 EPUB（zip 打不开）：{e}") from e

        self._names = {n.lower(): n for n in self.zf.namelist()}

        self.title = ""
        self.author = ""
        self.intro = ""
        self.cover_meta_id = ""
        self.manifest: dict[str, dict] = {}
        self.spine: list[dict] = []
        self.spine_pos: dict[str, int] = {}

        opf = self._find_opf()
        self.opf_dir = posixpath.dirname(opf)
        self._parse_opf(opf)

        if not self.spine:
            raise ConvertError("OPF 里没有 spine，无法确定正文顺序")

        for i, item in enumerate(self.spine):
            self.spine_pos.setdefault(item["path"], i)

    # ---------- 读取 ----------

    def _real(self, name: str) -> str:
        """zip 条目名可能和 href 大小写不一致，按小写索引找真实名字"""
        return self._names.get(name.lower(), "")

    def read(self, name: str) -> bytes:
        real = self._real(name)
        if not real:
            raise ConvertError(f"EPUB 里找不到文件：{name}")
        return self.zf.read(real)

    def _find_opf(self) -> str:
        try:
            raw = self.read("META-INF/container.xml")
            root = ET.fromstring(raw)
            for el in root.iter():
                if _local(el.tag) == "rootfile":
                    full = el.get("full-path", "")
                    if full and self._real(full):
                        return self._real(full)
        except (ConvertError, ET.ParseError):
            pass

        for n in self.zf.namelist():
            if n.lower().endswith(".opf"):
                return n
        raise ConvertError("找不到 OPF 文件，这可能不是 EPUB")

    def _parse_opf(self, opf_path: str) -> None:
        try:
            root = ET.fromstring(self.read(opf_path))
        except ET.ParseError as e:
            raise ConvertError(f"OPF 解析失败：{e}") from e

        meta_el = man_el = spine_el = None
        for el in root:
            name = _local(el.tag)
            if name == "metadata":
                meta_el = el
            elif name == "manifest":
                man_el = el
            elif name == "spine":
                spine_el = el

        if meta_el is not None:
            for ch in meta_el:
                name = _local(ch.tag)
                text = (ch.text or "").strip()
                if name == "title" and not self.title:
                    self.title = text
                elif name == "creator" and not self.author:
                    self.author = text
                elif name == "description" and not self.intro:
                    self.intro = ch.text or ""
                elif name == "meta" and (ch.get("name") or "").lower() == "cover":
                    self.cover_meta_id = ch.get("content", "")

        if man_el is None or spine_el is None:
            raise ConvertError("OPF 里缺少 manifest 或 spine")

        for it in man_el:
            if _local(it.tag) != "item":
                continue
            iid = it.get("id", "")
            href = it.get("href", "")
            if not iid or not href:
                continue
            real = self._real(resolve_href(self.opf_dir, href))
            self.manifest[iid] = {
                "href": href,
                "path": real,
                "media": it.get("media-type", ""),
                "props": (it.get("properties") or "").split(),
            }

        for it in spine_el:
            if _local(it.tag) != "itemref":
                continue
            item = self.manifest.get(it.get("idref", ""))
            if item and item["path"]:
                self.spine.append(item)

    # ---------- 目录 ----------

    def toc(self) -> list[TocNode]:
        nav = next((m for m in self.manifest.values() if "nav" in m["props"]), None)
        if nav and nav["path"]:
            nodes = self._toc_from_nav(nav)
            if nodes:
                return nodes

        # 退回 NCX：优先 spine 上声明的 toc id，其次 manifest 里的 ncx
        ncx = None
        for m in self.manifest.values():
            if m["media"] == "application/x-dtbncx+xml" and m["path"]:
                ncx = m
                break
        if ncx:
            nodes = parse_ncx(self.read(ncx["path"]))
            base = posixpath.dirname(ncx["path"])
            if nodes:
                self._resolve_nodes(nodes, base)
                return nodes
        return []

    def _toc_from_nav(self, nav: dict) -> list[TocNode]:
        parser = _NavParser()
        try:
            parser.feed(decode_bytes(self.read(nav["path"])))
            parser.close()
        except ConvertError:
            return []

        best: list[TocNode] = []
        for attrs, items in parser.navs:
            if not items:
                continue
            kind = (attrs.get("epub:type") or attrs.get("type") or "").lower()
            if "toc" in kind:
                best = items
                break
            if not best:
                best = items
        self._resolve_nodes(best, posixpath.dirname(nav["path"]))
        return best

    def _resolve_nodes(self, nodes: list[TocNode], base_dir: str) -> None:
        for n in nodes:
            n.title = _WS_RE.sub(" ", n.title).strip()
            if n.href:
                n.path = self._real(resolve_href(base_dir, n.href))
            self._resolve_nodes(n.children, base_dir)

    # ---------- 封面 ----------

    def cover_item(self) -> dict | None:
        # EPUB3：properties 里带 cover-image
        for m in self.manifest.values():
            if "cover-image" in m["props"] and m["path"] and m["media"].startswith("image/"):
                return m
        # EPUB2：<meta name="cover" content="id">
        if self.cover_meta_id:
            m = self.manifest.get(self.cover_meta_id)
            if m and m["path"] and m["media"].startswith("image/"):
                return m
        # 兜底：文件名里带 cover 的图片
        for m in self.manifest.values():
            if m["path"] and m["media"].startswith("image/") and "cover" in m["path"].lower():
                return m
        return None

    def meta(self) -> tuple[str, str, str, bool]:
        """
        取书名/作者/简介。返回 (书名, 作者, 简介, 简介是否被截断)。

        书名和作者会被截断 —— 它们是存在固件结构体里的（192/48 字节），
        超了就是真显示不下。简介不一样：它只是被存进 XML，展示时固件另外
        读全文，所以给的上限宽松得多（INTRO_MAX_BYTES）。
        """
        title = truncate_utf8(self.title or self.path.stem, TITLE_MAX_BYTES)
        author = truncate_utf8(self.author, AUTHOR_MAX_BYTES)
        # 段落之间**保留换行**：阅读器的简介层是可滚动的 label，认 \n，
        # 一段一行读起来才顺（压成一行会是一大坨）。段内的折行空格顺手去掉。
        intro = _CJK_SPACE_RE.sub("", html_text(self.intro))
        intro = re.sub(r"\n{2,}", "\n", intro).strip()
        cut = len(intro.encode("utf-8")) > INTRO_MAX_BYTES
        return title, author, truncate_utf8(intro, INTRO_MAX_BYTES), cut


# ===================== 章节切分 =====================

class Chapter:
    __slots__ = ("title", "volume", "start", "end", "text")

    def __init__(self, title: str, volume: str, start: int):
        self.title = title
        self.volume = volume
        self.start = start
        self.end = start
        self.text = ""


def _spine_index(book: EpubBook, node: TocNode) -> int | None:
    if not node.path:
        return None
    return book.spine_pos.get(node.path)


def build_chapters(book: EpubBook, blank_line: bool = False,
                   progress: ProgressCb | None = None) -> tuple[list[Chapter], bool]:
    """
    按 TOC 切章。返回 (章节列表, 是否用了 TOC)。

    规则：
      - 有子条目的目录节点 = 分卷，自身若指向独立内容会单独成一章（卷首语）
      - 叶子节点 = 章节
      - 指向同一个 spine 文件的多个条目只保留第一个（HTML 锚点无法在纯文本里切开）
      - TOC 覆盖不到的开头部分并入第一章，末尾部分并入最后一章
    """
    nodes = book.toc()
    raw: list[Chapter] = []

    def walk(items: list[TocNode], volume: str) -> None:
        for n in items:
            idx = _spine_index(book, n)
            if n.children:
                # 卷节点自己也有正文（且不在子章节的范围里）时，单独算一章
                child_idx = [i for i in (_spine_index(book, c) for c in _leaves(n)) if i is not None]
                if idx is not None and (not child_idx or idx < min(child_idx)):
                    raw.append(Chapter(n.title, volume, idx))
                walk(n.children, n.title or volume)
            elif idx is not None:
                raw.append(Chapter(n.title, volume, idx))

    if nodes:
        walk(nodes, "")

    raw.sort(key=lambda c: c.start)

    # 同一 spine 文件只保留第一个条目
    uniq: list[Chapter] = []
    for c in raw:
        if uniq and c.start == uniq[-1].start:
            continue
        uniq.append(c)

    if not uniq:
        return _chapters_from_spine(book, blank_line, progress), False

    # 定范围：本章起点 → 下一章起点
    for i, c in enumerate(uniq):
        c.end = uniq[i + 1].start if i + 1 < len(uniq) else len(book.spine)

    # TOC 第一条之前的正文（封面页/版权页/直接开写的开头）并进第一章
    if uniq[0].start > 0:
        uniq[0].start = 0

    chapters: list[Chapter] = []
    for i, c in enumerate(uniq):
        if progress:
            progress(i, len(uniq), f"排版第 {i + 1}/{len(uniq)} 章")
        text = _render_range(book, c.start, c.end, blank_line)
        if not text:
            continue                    # 纯图片页转出来是空的，丢掉，别留空章
        c.text = text
        chapters.append(c)

    if not chapters:
        return _chapters_from_spine(book, blank_line, progress), False
    return chapters, True


def _leaves(node: TocNode) -> list[TocNode]:
    if not node.children:
        return [node]
    out: list[TocNode] = []
    for c in node.children:
        out.extend(_leaves(c))
    return out


def _chapters_from_spine(book: EpubBook, blank_line: bool,
                         progress: ProgressCb | None = None) -> list[Chapter]:
    """没有可用目录时的退路：一个 xhtml 一章，标题取文件里的 <title>"""
    chapters: list[Chapter] = []
    for i, item in enumerate(book.spine):
        if progress:
            progress(i, len(book.spine), f"排版第 {i + 1}/{len(book.spine)} 个文件")
        text = _render_item(book, item, blank_line)
        if not text:
            continue
        c = Chapter(_html_title(book, item) or f"第 {len(chapters) + 1} 章", "", i)
        c.text = text
        chapters.append(c)
    return chapters


# ===================== 章节后处理 =====================

def _no_space(s: str) -> str:
    return re.sub(r"\s+", "", s)


def drop_dup_title(text: str, title: str) -> str:
    """
    正文开头那一行如果和章名重复就删掉（EPUB 里 <h1> 和目录条目常是同一句话）

    比较前把空白全部去掉，因为排版时中文字之间的空格已经被清理过，
    "第1章 陨落" 和 "第1章陨落" 得算同一行。
    """
    t = _no_space(title)
    if not t:
        return text

    lines = text.split("\n")
    i = 0
    while i < len(lines) and not lines[i].strip():
        i += 1
    if i >= len(lines):
        return text

    first = _no_space(lines[i])
    if not (first == t or first.startswith(t) or (t.startswith(first) and len(first) > 5)):
        return text

    del lines[i]
    while i < len(lines) and not lines[i].strip():
        del lines[i]
    return "\n".join(lines)


def filter_chapters(chapters: list[Chapter], skip: int = 0,
                    dedup_title: bool = False) -> tuple[list[Chapter], list[str]]:
    """
    可选的收尾处理，返回 (处理后的章节, 被跳过的章名列表)。

    skip：跳过前 N 章，用来丢掉简介、前言、导航这类开头的杂页
    dedup_title：去掉正文开头与章名重复的那一行
    """
    dropped: list[str] = []
    if skip > 0:
        dropped = [c.title or f"第 {i + 1} 章" for i, c in enumerate(chapters[:skip])]
        chapters = chapters[skip:]

    if dedup_title:
        for c in chapters:
            c.text = drop_dup_title(c.text, c.title)

    return chapters, dropped


def _html_title(book: EpubBook, item: dict) -> str:
    if not item["path"].lower().endswith((".xhtml", ".html", ".htm")):
        return ""
    try:
        raw = book.read(item["path"])
    except ConvertError:
        return ""
    head = raw[:4096].decode("utf-8", "replace")
    m = re.search(r"<title[^>]*>(.*?)</title>", head, re.S | re.I)
    if not m:
        return ""
    return _WS_RE.sub(" ", m.group(1)).strip()


def _render_range(book: EpubBook, start: int, end: int, blank_line: bool) -> str:
    parts: list[str] = []
    for item in book.spine[start:end]:
        t = _render_item(book, item, blank_line)
        if t:
            parts.append(t)
    return clean_text("\n".join(parts), blank_line)


def _render_item(book: EpubBook, item: dict, blank_line: bool) -> str:
    if not item["path"].lower().endswith((".xhtml", ".html", ".htm")):
        return ""                       # 图片等非正文项直接跳过
    try:
        return html_to_text(book.read(item["path"]), blank_line)
    except ConvertError:
        return ""


# ===================== 封面 =====================

def convert_cover(raw: bytes) -> tuple[int, int, bytes] | None:
    """图片字节 → (宽, 高, RGB565 小端字节流)；Pillow 缺失或解码失败返回 None"""
    try:
        from PIL import Image
    except ImportError:
        return None

    try:
        im = Image.open(io.BytesIO(raw))
        im.load()
        im = im.convert("RGB")
    except Exception:
        return None

    im.thumbnail((COVER_MAX_W, COVER_MAX_H))    # 只缩小，不放大小图
    w, h = im.size

    px = im.tobytes()                           # RGBRGB...
    out = bytearray(w * h * 2)
    for i in range(w * h):
        r = px[3 * i]
        g = px[3 * i + 1]
        b = px[3 * i + 2]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[2 * i] = v & 0xFF                   # 低字节在前（LVGL RGB565 的内存布局）
        out[2 * i + 1] = v >> 8
    return w, h, bytes(out)


# ===================== 输出 =====================

def write_ebf(book: EpubBook, chapters: list[Chapter],
              out_dir: Path, cover: tuple[int, int, bytes] | None,
              force: bool) -> None:
    if out_dir.exists():
        if not force:
            raise ConvertError(f"输出目录已存在：{out_dir}（加 --force 覆盖）")
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    # 章节文件：XML 首部（章名/卷名）+ 空行 + 正文原文
    #
    # 首部是给"只想知道这一章叫什么"用的 —— 固件读文件头几百字节就能拿到章名，
    # 不用去查索引。正文**不做 XML 转义**（< & 原样），所以首部和正文用空行隔开，
    # 固件解析到 </chapter> 之后跳过空行就是正文起点。这样正文不会膨胀，固件也
    # 不需要多加一层反转义。
    for i, c in enumerate(chapters, 1):
        head = ["<chapter>", f"<title>{xml_escape(c.title or f'第 {i} 章')}</title>"]
        if c.volume:
            head.append(f"<volume>{xml_escape(c.volume)}</volume>")
        head.append("</chapter>")
        (out_dir / CHAPTER_FMT.format(i)).write_text(
            "\n".join(head) + "\n\n" + c.text, encoding="utf-8", newline="\n")

    # 章名索引：一行一名，行序 = 章号（第 1 行就是第 1 章）。
    # **不写章号**：本工具保证 chapter.N.ebfl 从 1 连号，行序就已经是编号了，
    # 再存一份只会多一个可能对不上的地方。固件把它整份读进 PSRAM 当大纲用。
    (out_dir / "chapters.ebfl").write_text(
        "".join((c.title or f"第 {i} 章") + "\n" for i, c in enumerate(chapters, 1)),
        encoding="utf-8", newline="\n")

    # 封面
    if cover is not None:
        (out_dir / "cover.raw").write_bytes(cover[2])

    # 元数据
    title, author, intro, _ = book.meta()

    # <book chapters="N"> 让固件不用逐章探测就知道有多少章（挂 <book> 上是为了
    # 保证它落在固件只读的那几 KB 之内）
    lines = ['<?xml version="1.0" encoding="utf-8"?>',
             f'<book chapters="{len(chapters)}">']
    lines.append(f"<title>{xml_escape(title)}</title>")
    if author:
        lines.append(f"<author>{xml_escape(author)}</author>")
    if intro:
        lines.append(f"<intro>{xml_escape(intro)}</intro>")
    if cover is not None:
        lines.append(f'<cover width="{cover[0]}" height="{cover[1]}" '
                     f'file="cover.raw" format="rgb565le"/>')

    # 分卷表：卷名 + 章节范围。同一卷的章节在 TOC 里是连续的，压成 from/to 两段就够。
    # 章名单独存在 chapters.ebfl 里，所以这里只剩"哪几章属于哪一卷"。
    vols: list[list] = []
    for i, c in enumerate(chapters, 1):
        if vols and vols[-1][0] == c.volume:
            vols[-1][2] = i
        else:
            vols.append([c.volume, i, i])
    named = [v for v in vols if v[0]]
    if named:
        lines.append("<volumes>")
        for name, a, b in named:
            lines.append(f'<volume from="{a}" to="{b}">{xml_escape(name)}</volume>')
        lines.append("</volumes>")
    lines.append("</book>")

    (out_dir / "data.ebfl").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def html_text(s: str) -> str:
    """简介里常带 <p> 之类的标签，抽成纯文本"""
    if not s or "<" not in s:
        return s or ""
    p = HtmlToText()
    p.feed(s)
    p.close()
    return p.result()


# ===================== 主流程 =====================

@dataclass
class Options:
    """一次转换的全部参数，CLI 和 GUI 都构造它"""
    out_root: Path | None = None            # None = 放在 EPUB 同级目录
    force: bool = False                     # 输出目录已存在时覆盖
    blank_line: bool = False                # 段落之间空一行
    skip: int = 0                           # 跳过转换后的前 N 章
    dedup_title: bool = False               # 去掉正文开头与章名重复的那一行
    quiet: bool = False
    log: Callable[[str], None] | None = None        # 正常信息
    warn: Callable[[str], None] | None = None       # 警告/错误
    progress: ProgressCb | None = None

    def info(self, msg: str) -> None:
        if self.log is not None and not self.quiet:
            self.log(msg)

    def warning(self, msg: str) -> None:
        if self.warn is not None:
            self.warn(msg)
        elif not self.quiet:
            print(msg, file=sys.stderr)


def convert_one(src: Path, opt: Options) -> bool:
    try:
        book = EpubBook(src)
        chapters, used_toc = build_chapters(book, opt.blank_line, opt.progress)

        before = len(chapters)
        chapters, dropped = filter_chapters(chapters, opt.skip, opt.dedup_title)
        for name in dropped:
            opt.info(f"  跳过: {name}")
        if not chapters:
            if before == 0:
                raise ConvertError("没有抽到任何正文，可能是纯图片或加密的 EPUB")
            raise ConvertError(f"跳过前 {opt.skip} 章后就没有剩下了，请调小这个数")

        title, author, _, intro_cut = book.meta()
        out_dir = (opt.out_root or src.parent) / (safe_dir_name(title) + EBF_DIR_SUFFIX)

        if intro_cut:
            opt.warning(f"  [警告] 简介超过 {INTRO_MAX_BYTES} 字节，已截断")

        cover = None
        item = book.cover_item()
        if item:
            cover = convert_cover(book.read(item["path"]))
            if cover is None:
                opt.warning("  [警告] 封面跳过（没装 Pillow 或图片解不开）")

        write_ebf(book, chapters, out_dir, cover, opt.force)

        total = sum(len(c.text.encode("utf-8")) for c in chapters)
        opt.info(f"  书名: {title}")
        if author:
            opt.info(f"  作者: {author}")
        opt.info(f"  章节: {len(chapters)} 章"
                 f"（{'按目录切分' if used_toc else '无目录，按文件切分'}）")
        if cover is not None:
            opt.info(f"  封面: {cover[0]}x{cover[1]} RGB565，{len(cover[2]) / 1024:.0f} KB")
        else:
            opt.info("  封面: 无")
        size = f"{total / 1024:.1f} KB" if total >= 1024 else f"{total} 字节"
        opt.info(f"  正文: {size} UTF-8")

        for i, c in enumerate(chapters, 1):
            if len(c.text.encode("utf-8")) > CHAPTER_BYTES_WARN:
                opt.warning(f"  [警告] 第 {i} 章超过 4MB，阅读器可能打不开这章")

        opt.info(f"  输出: {out_dir}")
        return True
    except ConvertError as e:
        opt.warning(f"[失败] {src.name}: {e}")
        return False
    except Exception as e:                       # 单个文件出错不该中断批量转换
        opt.warning(f"[失败] {src.name}: {type(e).__name__}: {e}")
        return False


def collect_inputs(paths: list[str], warn: Callable[[str], None] | None = None) -> list[Path]:
    files: list[Path] = []
    for p in paths:
        path = Path(p)
        if path.is_dir():
            files.extend(sorted(x for x in path.rglob("*")
                                if x.suffix.lower() == ".epub" and x.is_file()))
        elif path.is_file():
            files.append(path)
        elif warn is not None:
            warn(f"[跳过] 找不到 {p}")
    return files


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="epub2ebf",
        description="把 EPUB 转成本项目的 EBF 书籍文件夹（chapter.N.ebfl + data.ebfl + cover.raw）",
        epilog="输出目录默认在 EPUB 同级的 <书名>.ebf/，加 -o 可统一指定。",
    )
    ap.add_argument("input", nargs="+", help="EPUB 文件或包含 EPUB 的目录")
    ap.add_argument("-o", "--out", help="输出根目录（默认放在 EPUB 旁边）")
    ap.add_argument("--force", action="store_true", help="输出目录已存在时覆盖")
    ap.add_argument("--blank-line", action="store_true", help="段落之间空一行")
    ap.add_argument("--skip", type=int, default=0, metavar="N",
                    help="跳过转换后的前 N 章（丢掉简介/前言这类开头页）")
    ap.add_argument("--dedup-title", action="store_true",
                    help="去掉正文开头与章名重复的那一行")
    ap.add_argument("-q", "--quiet", action="store_true", help="只报错误")
    args = ap.parse_args(argv)

    opt = Options(
        out_root=Path(args.out) if args.out else None,
        force=args.force,
        blank_line=args.blank_line,
        skip=max(0, args.skip),
        dedup_title=args.dedup_title,
        quiet=args.quiet,
        log=print,
    )

    files = collect_inputs(args.input, opt.warning)
    if not files:
        print("没有可转换的 EPUB", file=sys.stderr)
        return 1

    if opt.out_root is not None:
        opt.out_root.mkdir(parents=True, exist_ok=True)

    ok = 0
    for f in files:
        opt.info(f"[转换] {f}")
        if convert_one(f, opt):
            ok += 1

    if len(files) > 1:
        opt.info(f"完成：{ok}/{len(files)} 本")
    return 0 if ok == len(files) else 1


if __name__ == "__main__":
    sys.exit(main())
