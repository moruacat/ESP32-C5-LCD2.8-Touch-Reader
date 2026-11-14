package ebf

import (
	"fmt"
	"hash/fnv"
	"path/filepath"
	"strings"
)

// Options 是转换参数。
type Options struct {
	// BlankLine 段间空行（对应样本 EPUB 的 p{margin:.8em}）。
	BlankLine bool
	// Indent 段首两个全角空格（对应样本的 p{text-indent:2em}）。
	// 设备端阅读器不自动缩进，只能写进文本。
	Indent bool
	// Skip 跳过前 N 章（用来丢掉简介/前言这类开头杂页）。
	Skip int
	// DedupTitle 去掉正文开头与章名重复的那一行。
	DedupTitle bool
}

// Result 是一次转换的产出摘要。
type Result struct {
	Title    string
	Author   string
	Chapters int
	Bytes    int64
	Dir      string
	UsedTOC  bool
	Cover    *Cover
	Warnings []string
	// ChapterList 供上层写 chapters 表（覆盖更新时按 (no, hash) 对比）。
	ChapterList []ChapterInfo
}

// ChapterInfo 是章节索引项。
type ChapterInfo struct {
	No    int
	Title string
	Bytes int
	Hash  string
}

// Convert 把 epubPath 转成 EBF 目录写到 outDir。
func Convert(epubPath, outDir string, opt Options) (*Result, error) {
	book, err := OpenBook(epubPath)
	if err != nil {
		return nil, err
	}
	defer book.Close()

	chapters, usedTOC := buildChapters(book, opt)
	chapters, _ = filterChapters(chapters, opt)

	// 摘掉「简介」「目录」这类前置页：它们不是正文，独立成章会让书架里
	// 多出两章垃圾（目录页尤其长，全是章节名重复一遍）。
	var frontIntro string
	chapters = dropFrontMatter(chapters, &frontIntro)

	if len(chapters) == 0 {
		return nil, fmt.Errorf("没有抽到任何正文，可能是纯图片或加密的 EPUB")
	}

	fallback := strings.TrimSuffix(filepath.Base(epubPath), filepath.Ext(epubPath))
	title, author, intro, introCut := book.Meta(fallback)

	// OPF 没写 description 时，用「简介」页的正文顶上
	if strings.TrimSpace(intro) == "" && strings.TrimSpace(frontIntro) != "" {
		intro = strings.TrimSpace(frontIntro)
		if len(intro) > IntroMaxBytes {
			intro = truncateUTF8(intro, IntroMaxBytes)
			introCut = true
		}
	}

	res := &Result{Title: title, Author: author, UsedTOC: usedTOC}
	if introCut {
		res.Warnings = append(res.Warnings, fmt.Sprintf("简介超过 %d 字节，已截断", IntroMaxBytes))
	}

	// 封面
	if item := book.CoverItem(); item != nil {
		if raw, err := book.Read(item.Path); err == nil {
			if w, h, data, ok := convertCover(raw); ok {
				res.Cover = &Cover{W: w, H: h, Data: data}
			} else {
				res.Warnings = append(res.Warnings, "封面跳过：图片解不开")
			}
		}
	}

	// 章名缺省补「第 N 章」
	for i := range chapters {
		if strings.TrimSpace(chapters[i].Title) == "" {
			chapters[i].Title = fmt.Sprintf("第 %d 章", i+1)
		}
	}

	if err := writeEBF(outDir, chapters, title, author, intro, res.Cover); err != nil {
		return nil, err
	}

	res.Chapters = len(chapters)
	res.Dir = outDir
	for i, c := range chapters {
		res.Bytes += int64(len(c.Text))
		res.ChapterList = append(res.ChapterList, ChapterInfo{
			No:    i + 1,
			Title: c.Title,
			Bytes: len(c.Text),
			Hash:  contentHash(c.Text),
		})
		if len(c.Text) > 4*1024*1024 {
			res.Warnings = append(res.Warnings,
				fmt.Sprintf("第 %d 章超过 4MB，阅读器可能打不开", i+1))
		}
	}
	return res, nil
}

// contentHash 给章节内容算一个短哈希（覆盖更新时判断某章是否变过）。
func contentHash(s string) string {
	h := fnv.New64a()
	_, _ = h.Write([]byte(s))
	return fmt.Sprintf("%x", h.Sum64())
}

// buildChapters 按 TOC 切章；TOC 不可用时退回按 spine 每文件一章。
func buildChapters(book *Book, opt Options) ([]Chapter, bool) {
	nodes := book.Toc()

	var raw []Chapter
	var walk func(items []TocNode, volume string)
	walk = func(items []TocNode, volume string) {
		for _, n := range items {
			idx, hasIdx := book.spinePos[n.Path]
			if len(n.Children) > 0 {
				// 卷节点自身有正文、且早于所有子章节时，单独算一章（卷首语）
				minChild := -1
				for _, leaf := range leaves(n) {
					if i, ok := book.spinePos[leaf.Path]; ok {
						if minChild < 0 || i < minChild {
							minChild = i
						}
					}
				}
				if hasIdx && (minChild < 0 || idx < minChild) {
					raw = append(raw, Chapter{Title: n.Title, Volume: volume, Start: idx})
				}
				walk(n.Children, n.Title)
			} else if hasIdx {
				raw = append(raw, Chapter{Title: n.Title, Volume: volume, Start: idx})
			}
		}
	}
	if len(nodes) > 0 {
		walk(nodes, "")
	}

	// 按 spine 位置排序
	for i := 1; i < len(raw); i++ {
		for j := i; j > 0 && raw[j].Start < raw[j-1].Start; j-- {
			raw[j], raw[j-1] = raw[j-1], raw[j]
		}
	}

	// 同一 spine 文件只保留第一条（纯文本切不开锚点）
	var uniq []Chapter
	for _, c := range raw {
		if len(uniq) > 0 && c.Start == uniq[len(uniq)-1].Start {
			continue
		}
		uniq = append(uniq, c)
	}
	if len(uniq) == 0 {
		return chaptersFromSpine(book, opt), false
	}

	// 定范围
	for i := range uniq {
		if i+1 < len(uniq) {
			uniq[i].End = uniq[i+1].Start
		} else {
			uniq[i].End = len(book.Spine)
		}
	}
	// TOC 之前的正文并入首章
	uniq[0].Start = 0

	var out []Chapter
	for _, c := range uniq {
		text := renderRange(book, c.Start, c.End, opt)
		if text == "" {
			continue // 纯图片页，丢掉空章
		}
		c.Text = text
		out = append(out, c)
	}
	if len(out) == 0 {
		return chaptersFromSpine(book, opt), false
	}
	return out, true
}

// leaves 取一个节点的全部叶子。
func leaves(n TocNode) []TocNode {
	if len(n.Children) == 0 {
		return []TocNode{n}
	}
	var out []TocNode
	for _, c := range n.Children {
		out = append(out, leaves(c)...)
	}
	return out
}

// chaptersFromSpine 没有可用目录时的退路：一个 xhtml 一章，标题取 <title>。
func chaptersFromSpine(book *Book, opt Options) []Chapter {
	var chapters []Chapter
	for i, item := range book.Spine {
		text := renderItem(book, item, opt)
		if text == "" {
			continue
		}
		title := htmlTitle(book, item)
		if title == "" {
			title = fmt.Sprintf("第 %d 章", len(chapters)+1)
		}
		chapters = append(chapters, Chapter{Title: title, Start: i, Text: text})
	}
	return chapters
}

// renderRange 渲染 spine[start:end] 的全部正文。
func renderRange(book *Book, start, end int, opt Options) string {
	if start < 0 {
		start = 0
	}
	if end > len(book.Spine) {
		end = len(book.Spine)
	}
	if start >= end {
		return ""
	}
	var parts []string
	for _, item := range book.Spine[start:end] {
		if t := renderItem(book, item, opt); t != "" {
			parts = append(parts, t)
		}
	}
	text := cleanText(strings.Join(parts, "\n"), opt.BlankLine)
	if opt.Indent {
		text = indentParagraphs(text)
	}
	return text
}

// renderItem 渲染单个 spine 项（非 xhtml 直接跳过）。
func renderItem(book *Book, item *ManifestItem, opt Options) string {
	lower := strings.ToLower(item.Path)
	if !strings.HasSuffix(lower, ".xhtml") && !strings.HasSuffix(lower, ".html") &&
		!strings.HasSuffix(lower, ".htm") {
		return ""
	}
	raw, err := book.Read(item.Path)
	if err != nil {
		return ""
	}
	text := htmlToText(raw, opt.BlankLine)
	if opt.Indent {
		text = indentParagraphs(text)
	}
	return text
}

// htmlTitle 从文件头找 <title>。
func htmlTitle(book *Book, item *ManifestItem) string {
	raw, err := book.Read(item.Path)
	if err != nil {
		return ""
	}
	if len(raw) > 4096 {
		raw = raw[:4096]
	}
	s := string(raw)
	i := strings.Index(strings.ToLower(s), "<title")
	if i < 0 {
		return ""
	}
	j := strings.Index(s[i:], ">")
	if j < 0 {
		return ""
	}
	rest := s[i+j+1:]
	k := strings.Index(strings.ToLower(rest), "</title>")
	if k < 0 {
		return ""
	}
	return strings.TrimSpace(wsRe.ReplaceAllString(rest[:k], " "))
}

// filterChapters 可选收尾：跳过前 N 章、去掉正文开头重复的章名。
func filterChapters(chapters []Chapter, opt Options) ([]Chapter, []string) {
	var dropped []string
	if opt.Skip > 0 && opt.Skip <= len(chapters) {
		for _, c := range chapters[:opt.Skip] {
			dropped = append(dropped, c.Title)
		}
		chapters = chapters[opt.Skip:]
	}
	if opt.DedupTitle {
		for i := range chapters {
			chapters[i].Text = dropDupTitle(chapters[i].Text, chapters[i].Title)
		}
	}
	return chapters, dropped
}

// dropDupTitle 正文开头那行如果和章名重复就删掉（比较时忽略全部空白）。
func dropDupTitle(text, title string) string {
	if text == "" || title == "" {
		return text
	}
	idx := strings.IndexByte(text, '\n')
	first := text
	rest := ""
	if idx >= 0 {
		first = text[:idx]
		rest = text[idx+1:]
	}
	noSpace := func(s string) string {
		return strings.Map(func(r rune) rune {
			if r == ' ' || r == '\t' || r == '\u3000' {
				return -1
			}
			return r
		}, s)
	}
	if noSpace(first) != noSpace(title) {
		return text
	}
	// 去掉重复行和紧随其后的空行
	return strings.TrimLeft(rest, "\n")
}

// dropFrontMatter 摘掉「简介」「目录」这类前置页。
//
// 样本 EPUB 的 TOC 会把它们登记成章节，不处理的话书架里会多出
// 「简介」「目录」两章，其中目录页是全部章节名的重复。
// 简介页的正文回填到 *intro（OPF 没写 description 时才用）。
func dropFrontMatter(chapters []Chapter, intro *string) []Chapter {
	var kept []Chapter
	for _, c := range chapters {
		t := strings.Join(strings.Fields(c.Title), "")
		switch {
		case isIntroTitle(t):
			if strings.TrimSpace(*intro) == "" {
				*intro = strings.TrimSpace(c.Text)
			}
			continue
		case isTocTitle(t):
			continue
		}
		kept = append(kept, c)
	}
	return kept
}

// isIntroTitle 判断是不是简介页（比较时已去掉全部空白）。
func isIntroTitle(t string) bool {
	switch t {
	case "简介", "内容简介", "作品简介", "书籍简介", "本书简介":
		return true
	}
	return false
}

// isTocTitle 判断是不是目录页。
func isTocTitle(t string) bool {
	switch t {
	case "目录", "目次", "CONTENTS", "Contents", "contents":
		return true
	}
	return false
}

// indentParagraphs 给每个非空段加两个全角空格。
func indentParagraphs(text string) string {
	if text == "" {
		return ""
	}
	lines := strings.Split(text, "\n")
	for i, line := range lines {
		if strings.TrimSpace(line) == "" {
			continue
		}
		lines[i] = "　　" + line
	}
	return strings.Join(lines, "\n")
}
