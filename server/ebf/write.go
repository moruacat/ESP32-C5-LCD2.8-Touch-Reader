package ebf

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// Chapter 是一章的输出单元。
type Chapter struct {
	Title  string
	Volume string
	Start  int // spine 起点（含）
	End    int // spine 终点（不含）
	Text   string
}

// Cover 是处理好的封面。
type Cover struct {
	W, H int
	Data []byte
}

// xmlEscape 与 Python 版一致：只转义 & < > "。
func xmlEscape(s string) string {
	r := strings.NewReplacer("&", "&amp;", "<", "&lt;", ">", "&gt;", `"`, "&quot;")
	return r.Replace(s)
}

// writeEBF 生成 EBF 目录。outDir 若已存在会被清空重建（覆盖更新由上层决定）。
func writeEBF(outDir string, chapters []Chapter, title, author, intro string, cover *Cover) error {
	if err := os.RemoveAll(outDir); err != nil {
		return fmt.Errorf("清理输出目录失败: %w", err)
	}
	if err := os.MkdirAll(outDir, 0o755); err != nil {
		return fmt.Errorf("创建输出目录失败: %w", err)
	}

	// 章节文件：XML 首部 + 空行 + 正文原文（正文不做 XML 转义）
	for i, c := range chapters {
		name := c.Title
		if name == "" {
			name = fmt.Sprintf("第 %d 章", i+1)
		}
		var head strings.Builder
		head.WriteString("<chapter>\n")
		head.WriteString("<title>" + xmlEscape(name) + "</title>\n")
		if c.Volume != "" {
			head.WriteString("<volume>" + xmlEscape(c.Volume) + "</volume>\n")
		}
		head.WriteString("</chapter>\n\n")

		content := head.String() + c.Text
		if err := os.WriteFile(filepath.Join(outDir, chapterFileName(i+1)),
			[]byte(content), 0o644); err != nil {
			return fmt.Errorf("写第 %d 章失败: %w", i+1, err)
		}
	}

	// 章名索引：一行一名，行序 = 章号
	var idx strings.Builder
	for i, c := range chapters {
		name := c.Title
		if name == "" {
			name = fmt.Sprintf("第 %d 章", i+1)
		}
		idx.WriteString(name)
		idx.WriteString("\n")
	}
	if err := os.WriteFile(filepath.Join(outDir, "chapters.ebfl"),
		[]byte(idx.String()), 0o644); err != nil {
		return fmt.Errorf("写章节索引失败: %w", err)
	}

	// 封面
	if cover != nil && len(cover.Data) > 0 {
		if err := os.WriteFile(filepath.Join(outDir, "cover.raw"), cover.Data, 0o644); err != nil {
			return fmt.Errorf("写封面失败: %w", err)
		}
	}

	// 元数据
	var lines []string
	lines = append(lines,
		`<?xml version="1.0" encoding="utf-8"?>`,
		fmt.Sprintf(`<book chapters="%d">`, len(chapters)))
	lines = append(lines, "<title>"+xmlEscape(title)+"</title>")
	if author != "" {
		lines = append(lines, "<author>"+xmlEscape(author)+"</author>")
	}
	if intro != "" {
		lines = append(lines, "<intro>"+xmlEscape(intro)+"</intro>")
	}
	if cover != nil && len(cover.Data) > 0 {
		lines = append(lines, fmt.Sprintf(
			`<cover width="%d" height="%d" file="cover.raw" format="rgb565le"/>`,
			cover.W, cover.H))
	}

	// 分卷表：同一卷的章节在 TOC 里连续，压成 from/to 两段
	type volRange struct {
		name string
		from int
		to   int
	}
	var vols []volRange
	for i, c := range chapters {
		no := i + 1
		if n := len(vols); n > 0 && vols[n-1].name == c.Volume {
			vols[n-1].to = no
		} else {
			vols = append(vols, volRange{c.Volume, no, no})
		}
	}
	var named []volRange
	for _, v := range vols {
		if v.name != "" {
			named = append(named, v)
		}
	}
	if len(named) > 0 {
		lines = append(lines, "<volumes>")
		for _, v := range named {
			lines = append(lines, fmt.Sprintf(`<volume from="%d" to="%d">%s</volume>`,
				v.from, v.to, xmlEscape(v.name)))
		}
		lines = append(lines, "</volumes>")
	}
	lines = append(lines, "</book>")

	if err := os.WriteFile(filepath.Join(outDir, "data.ebfl"),
		[]byte(strings.Join(lines, "\n")+"\n"), 0o644); err != nil {
		return fmt.Errorf("写元数据失败: %w", err)
	}
	return nil
}
