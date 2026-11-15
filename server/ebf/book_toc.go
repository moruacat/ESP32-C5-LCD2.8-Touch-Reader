package ebf

import (
	"encoding/xml"
	"fmt"
	"path"
	"strings"
	"unicode/utf8"

	"golang.org/x/net/html"
)

// ---------- 文本截断 ----------

// TruncateUTF8 按字节截断且不切出半个汉字（供非 EPUB 来源复用）。
func TruncateUTF8(s string, maxBytes int) string {
	return truncateUTF8(s, maxBytes)
}

// truncateUTF8 按字节截断且不切出半个汉字（对应 Python 的 truncate_utf8）。
func truncateUTF8(s string, maxBytes int) string {
	if len(s) <= maxBytes {
		return s
	}
	b := []byte(s)[:maxBytes]
	for len(b) > 0 {
		r, size := utf8.DecodeLastRune(b)
		if r != utf8.RuneError || size > 1 {
			return string(b)
		}
		b = b[:len(b)-1]
	}
	return ""
}

// ---------- 目录 ----------

// Toc 返回目录树：优先 EPUB3 的 nav.xhtml，退回 EPUB2 的 NCX。
func (b *Book) Toc() []TocNode {
	// nav.xhtml（manifest properties 里带 nav）
	for _, m := range b.Manifest {
		if m.Path == "" {
			continue
		}
		hasNav := false
		for _, p := range m.Props {
			if p == "nav" {
				hasNav = true
			}
		}
		if !hasNav {
			continue
		}
		if nodes := b.tocFromNav(m); len(nodes) > 0 {
			return nodes
		}
	}

	// NCX
	for _, m := range b.Manifest {
		if m.Media == "application/x-dtbncx+xml" && m.Path != "" {
			raw, err := b.Read(m.Path)
			if err != nil {
				continue
			}
			nodes := parseNCX(raw)
			if len(nodes) > 0 {
				b.resolveNodes(nodes, path.Dir(m.Path))
				return nodes
			}
		}
	}
	return nil
}

// tocFromNav 解析 nav.xhtml 里的 <ol><li><a> 层级。
func (b *Book) tocFromNav(item *ManifestItem) []TocNode {
	raw, err := b.Read(item.Path)
	if err != nil {
		return nil
	}
	doc, err := html.Parse(strings.NewReader(string(raw)))
	if err != nil {
		return nil
	}

	// 找 <nav>：优先 epub:type 含 toc 的那个，没有就用第一个 nav
	var firstNav, tocNav *html.Node
	var findNav func(*html.Node)
	findNav = func(n *html.Node) {
		if n.Type == html.ElementNode && strings.EqualFold(n.Data, "nav") {
			if firstNav == nil {
				firstNav = n
			}
			kind := strings.ToLower(attrValue(n, "epub:type"))
			if kind == "" {
				kind = strings.ToLower(attrValue(n, "type"))
			}
			if tocNav == nil && strings.Contains(kind, "toc") {
				tocNav = n
			}
		}
		for c := n.FirstChild; c != nil; c = c.NextSibling {
			findNav(c)
		}
	}
	findNav(doc)

	nav := tocNav
	if nav == nil {
		nav = firstNav
	}
	if nav == nil {
		return nil
	}

	nodes := parseNavList(nav)
	b.resolveNodes(nodes, path.Dir(item.Path))
	return nodes
}

// parseNavList 递归解析 <ol>/<li>/<a>。
func parseNavList(n *html.Node) []TocNode {
	var out []TocNode
	var walkOl func(*html.Node)
	walkOl = func(ol *html.Node) {
		for li := ol.FirstChild; li != nil; li = li.NextSibling {
			if li.Type != html.ElementNode || !strings.EqualFold(li.Data, "li") {
				continue
			}
			node := TocNode{}
			// 当前 li 下第一层 <a>
			var scanLi func(*html.Node)
			scanLi = func(x *html.Node) {
				for c := x.FirstChild; c != nil; c = c.NextSibling {
					if c.Type != html.ElementNode {
						continue
					}
					switch strings.ToLower(c.Data) {
					case "a":
						if node.Href == "" {
							node.Href = attrValue(c, "href")
							node.Title = strings.TrimSpace(textOf(c))
						}
					case "ol":
						node.Children = append(node.Children, parseNavList(c)...)
					case "span":
						if node.Href == "" {
							node.Href = attrValue(c, "href")
							node.Title = strings.TrimSpace(textOf(c))
						}
					}
				}
			}
			scanLi(li)
			if node.Title != "" || node.Href != "" || len(node.Children) > 0 {
				out = append(out, node)
			}
		}
	}

	// nav 里通常直接是 <ol>
	var findOl func(*html.Node)
	findOl = func(x *html.Node) {
		for c := x.FirstChild; c != nil; c = c.NextSibling {
			if c.Type == html.ElementNode && strings.EqualFold(c.Data, "ol") {
				walkOl(c)
				return
			}
			findOl(c)
		}
	}
	findOl(n)
	return out
}

// ---------- NCX ----------

type ncxDoc struct {
	NavMap struct {
		Points []ncxPoint `xml:"navPoint"`
	} `xml:"navMap"`
}

type ncxPoint struct {
	Label struct {
		Text string `xml:"text"`
	} `xml:"navLabel"`
	Content struct {
		Src string `xml:"src,attr"`
	} `xml:"content"`
	Points []ncxPoint `xml:"navPoint"`
}

func parseNCX(raw []byte) []TocNode {
	var doc ncxDoc
	if err := xml.Unmarshal(raw, &doc); err != nil {
		return nil
	}
	var conv func([]ncxPoint) []TocNode
	conv = func(points []ncxPoint) []TocNode {
		var out []TocNode
		for _, p := range points {
			out = append(out, TocNode{
				Title:    strings.TrimSpace(p.Label.Text),
				Href:     p.Content.Src,
				Children: conv(p.Points),
			})
		}
		return out
	}
	return conv(doc.NavMap.Points)
}

// resolveNodes 把 href 解析成 zip 路径，并规整标题空白。
func (b *Book) resolveNodes(nodes []TocNode, baseDir string) {
	for i := range nodes {
		n := &nodes[i]
		n.Title = strings.TrimSpace(wsRe.ReplaceAllString(n.Title, " "))
		if n.Href != "" {
			n.Path = b.real(resolveHref(baseDir, n.Href))
		}
		b.resolveNodes(n.Children, baseDir)
	}
}

// ---------- 封面与元数据 ----------

// CoverItem 取封面图：EPUB3 的 cover-image → EPUB2 的 meta[name=cover] → 文件名兜底。
func (b *Book) CoverItem() *ManifestItem {
	for _, m := range b.Manifest {
		for _, p := range m.Props {
			if p == "cover-image" && m.Path != "" && strings.HasPrefix(m.Media, "image/") {
				return m
			}
		}
	}
	if b.CoverMeta != "" {
		if m, ok := b.Manifest[b.CoverMeta]; ok && m.Path != "" &&
			strings.HasPrefix(m.Media, "image/") {
			return m
		}
	}
	for _, m := range b.Manifest {
		if m.Path != "" && strings.HasPrefix(m.Media, "image/") &&
			strings.Contains(strings.ToLower(m.Path), "cover") {
			return m
		}
	}
	return nil
}

// Meta 返回书名/作者/简介（都已按固件上限截断）以及简介是否被截断。
func (b *Book) Meta(fallbackTitle string) (title, author, intro string, introCut bool) {
	title = truncateUTF8(strings.TrimSpace(b.Title), TitleMaxBytes)
	if title == "" {
		title = truncateUTF8(fallbackTitle, TitleMaxBytes)
	}
	author = truncateUTF8(strings.TrimSpace(b.Author), AuthorMaxBytes)

	// 简介保留换行（阅读器的简介层是可滚动 label），去掉段内折行空格与多余空行
	intro = stripCJKSpace(htmlToText([]byte(b.Intro), false))
	intro = strings.TrimSpace(compressBlankLines(intro))
	if len(intro) > IntroMaxBytes {
		intro = truncateUTF8(intro, IntroMaxBytes)
		introCut = true
	}
	return title, author, intro, introCut
}

// compressBlankLines 把连续空行压成一个空行。
func compressBlankLines(s string) string {
	lines := strings.Split(s, "\n")
	var out []string
	prevBlank := true
	for _, l := range lines {
		blank := strings.TrimSpace(l) == ""
		if blank {
			if !prevBlank {
				out = append(out, "")
			}
			prevBlank = true
			continue
		}
		out = append(out, l)
		prevBlank = false
	}
	return strings.Join(out, "\n")
}

// ---------- 小工具 ----------

func attrValue(n *html.Node, key string) string {
	for _, a := range n.Attr {
		if strings.EqualFold(a.Key, key) {
			return a.Val
		}
	}
	return ""
}

func textOf(n *html.Node) string {
	var b strings.Builder
	var walk func(*html.Node)
	walk = func(x *html.Node) {
		if x.Type == html.TextNode {
			b.WriteString(x.Data)
		}
		for c := x.FirstChild; c != nil; c = c.NextSibling {
			walk(c)
		}
	}
	walk(n)
	return b.String()
}

// 与固件对齐的常量
const (
	TitleMaxBytes  = 180 // srv_library 的 SRV_BOOK_NAME_MAX = 192
	AuthorMaxBytes = 44  // SRV_BOOK_AUTHOR_MAX = 48
	IntroMaxBytes  = 2000
	EbfDirSuffix   = ".ebf"
)

// chapterFileName 生成 chapter.N.ebfl。
func chapterFileName(n int) string { return fmt.Sprintf("chapter.%d.ebfl", n) }
