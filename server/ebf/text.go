package ebf

import (
	"bytes"
	"regexp"
	"strings"

	"golang.org/x/net/html"
)

// 与 Python 版一致：这些标签整块跳过。
var skipTags = map[string]bool{
	"script": true, "style": true, "head": true, "title": true,
	"svg": true, "math": true, "noscript": true, "template": true,
	"meta": true, "link": true, "canvas": true, "iframe": true, "object": true,
}

// 块级标签：边界留一个换行。
var blockTags = map[string]bool{
	"p": true, "div": true, "section": true, "article": true, "aside": true,
	"header": true, "footer": true, "main": true, "nav": true, "blockquote": true,
	"pre": true, "figure": true, "figcaption": true,
	"ul": true, "ol": true, "li": true, "dl": true, "dt": true, "dd": true,
	"table": true, "thead": true, "tbody": true, "tfoot": true, "tr": true,
	"td": true, "th": true, "center": true, "address": true, "fieldset": true,
	"form": true, "details": true, "summary": true, "caption": true,
}

var headingTags = map[string]bool{
	"h1": true, "h2": true, "h3": true, "h4": true, "h5": true, "h6": true,
}

// wsRe 对应 Python 的 _WS_RE：空白（含不换行空格、全角空格）折叠成一个空格。
var wsRe = regexp.MustCompile(`[ \t\r\n\f\v\x{00a0}\x{3000}]+`)

// isCJK 对应 Python 的 _CJK 字符类。
func isCJK(r rune) bool {
	return (r >= 0x2e80 && r <= 0x4dbf) || // CJK 部首扩展
		(r >= 0x4e00 && r <= 0x9fff) || // CJK 基本区
		(r >= 0x3000 && r <= 0x303f) || // 中文标点
		(r >= 0xff00 && r <= 0xffef) // 全角字符
}

// stripCJKSpace 删掉「两个 CJK 字符之间」的单个空格。
// Go 的 RE2 不支持 lookbehind，所以手写扫描（等价于 Python 的 _CJK_SPACE_RE）。
func stripCJKSpace(s string) string {
	if !strings.Contains(s, " ") {
		return s
	}
	runes := []rune(s)
	out := make([]rune, 0, len(runes))
	for i, r := range runes {
		if r == ' ' && i > 0 && i+1 < len(runes) &&
			isCJK(runes[i-1]) && isCJK(runes[i+1]) {
			continue
		}
		out = append(out, r)
	}
	return string(out)
}

// htmlToText 把一章的 xhtml 抽成纯文本。
// 与 Python 版 HtmlToText + clean_text 行为一致。
func htmlToText(raw []byte, blankLine bool) string {
	doc, err := html.Parse(bytes.NewReader(raw))
	if err != nil {
		return ""
	}

	var parts strings.Builder
	pending := 0 // 待插入的换行数（合并连续块级标签，避免多空行）

	addNL := func(n int) {
		if n > pending {
			pending = n
		}
	}
	flushNL := func() {
		if pending > 0 {
			parts.WriteString(strings.Repeat("\n", pending))
			pending = 0
		}
	}

	var walk func(n *html.Node)
	walk = func(n *html.Node) {
		if n.Type == html.ElementNode {
			tag := strings.ToLower(n.Data)
			if skipTags[tag] {
				return // 整棵子树跳过（script/style/head...）
			}
			switch {
			case tag == "br":
				addNL(1)
			case headingTags[tag] || tag == "hr":
				addNL(2)
			case blockTags[tag]:
				addNL(1)
			}
		}

		if n.Type == html.TextNode {
			text := wsRe.ReplaceAllString(n.Data, " ")
			if strings.TrimSpace(text) != "" {
				flushNL()
				parts.WriteString(text)
			}
		}

		for c := n.FirstChild; c != nil; c = c.NextSibling {
			walk(c)
		}

		// 结束标签的换行（与开始标签对称）
		if n.Type == html.ElementNode {
			tag := strings.ToLower(n.Data)
			if skipTags[tag] {
				return
			}
			if headingTags[tag] || tag == "hr" {
				addNL(2)
			} else if blockTags[tag] {
				addNL(1)
			}
		}
	}
	walk(doc)

	flushNL()
	return cleanText(parts.String(), blankLine)
}

// cleanText 规整文本：一行一段、去掉多余空白、可选段首缩进与段间空行。
//
// indent=true 时给每段加两个全角空格——设备端阅读器不自动缩进，
// 想复刻样本 EPUB 的 text-indent:2em 只能写进字符里。
func cleanText(s string, blankLine bool) string {
	s = strings.ReplaceAll(s, "\r\n", "\n")
	s = strings.ReplaceAll(s, "\r", "\n")

	var lines []string
	for _, raw := range strings.Split(s, "\n") {
		line := strings.TrimSpace(wsRe.ReplaceAllString(raw, " "))
		line = stripCJKSpace(line)
		lines = append(lines, line)
	}

	// 连续空行压成一个
	var out []string
	blank := false
	for _, line := range lines {
		if line != "" {
			out = append(out, line)
			blank = false
		} else if len(out) > 0 && !blank {
			out = append(out, "")
			blank = true
		}
	}

	if len(out) == 0 {
		return ""
	}

	sep := "\n"
	if blankLine {
		sep = "\n\n"
	}

	var b strings.Builder
	for i, line := range out {
		if line == "" {
			continue
		}
		if i > 0 {
			b.WriteString(sep)
		}
		b.WriteString(line)
	}
	b.WriteString("\n")
	return b.String()
}
