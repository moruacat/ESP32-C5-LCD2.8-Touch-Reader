package fanqie

// App 接口的对外封装。
//
// 与网页接口的区别：App 端（reading.snssdk.com）的章节权限规则更宽，
// 手机 App 里能看到的章节这里也能取到，适合"App 能看但网页不给"的书。
// 请求需要签名（appsign.go），正文是 AES 密文（appapi.go 负责解）。

import (
	"fmt"
	"strings"

	"clockreader-server/ebf"
	"time"
)

// AppCatalog 用 App 接口拉目录。
func AppCatalog(bookID string) ([]Chapter, error) {
	return newAppClient().appCatalog(bookID)
}

// AppChapter 用 App 接口拉一章正文并解密，返回 (标题, 正文)。
func AppChapter(itemID string) (string, string, error) {
	title, body, err := newAppClient().fetchChapter(itemID)
	if err != nil {
		return title, "", err
	}
	body = strings.TrimSpace(body)
	if body == "" {
		return title, "", fmt.Errorf("正文为空")
	}
	return title, body, nil
}

// AppImport 用 App 接口把一本书抓成 EBF。
func AppImport(book Book, outDir string, opt ImportOptions) (*ebf.Result, error) {
	cli := newAppClient()

	dir, err := cli.appCatalog(book.BookID)
	if err != nil {
		return nil, err
	}

	total := len(dir)
	if opt.MaxChapters > 0 && opt.MaxChapters < total {
		total = opt.MaxChapters
	}

	out := make([]ebf.Chapter, 0, total)
	var warnings []string
	var failed int

	for i := 0; i < total; i++ {
		ch := dir[i]

		title, body, err := cli.fetchChapter(ch.ItemID)
		if err != nil {
			failed++
			warnings = append(warnings, fmt.Sprintf("第 %d 章失败：%v", i+1, err))
			continue
		}
		if title == "" {
			title = ch.Title
		}

		volume := ch.Volume
		if volume == "第一卷：默认" {
			volume = ""
		}

		out = append(out, ebf.Chapter{
			Title:  title,
			Volume: volume,
			Text:   body + "\n",
		})

		if opt.Progress != nil {
			opt.Progress(i+1, total, title)
		}
		if opt.Delay > 0 {
			time.Sleep(opt.Delay)
		}
	}

	if len(out) == 0 {
		return nil, fmt.Errorf("没有任何可下载的章节（全部失败）")
	}

	intro := book.Abstract
	if len(intro) > ebf.IntroMaxBytes {
		intro = ebf.TruncateUTF8(intro, ebf.IntroMaxBytes)
	}

	var cover *ebf.Cover
	if book.CoverURL != "" {
		if raw, err := NewClient().Download(book.CoverURL); err == nil {
			if cv, ok := ebf.ConvertCover(raw); ok {
				cover = cv
			}
		}
	}

	if err := ebf.WriteDir(outDir, out, book.Title, book.Author, intro, cover); err != nil {
		return nil, err
	}

	res := &ebf.Result{
		Title:    book.Title,
		Author:   book.Author,
		Chapters: len(out),
		Dir:      outDir,
		Cover:    cover,
		Warnings: warnings,
	}
	for i, c := range out {
		res.Bytes += int64(len(c.Text))
		res.ChapterList = append(res.ChapterList, ebf.ChapterInfo{
			No: i + 1, Title: c.Title, Bytes: len(c.Text), Hash: contentHash(c.Text),
		})
	}
	if failed > 0 {
		res.Warnings = append(res.Warnings,
			fmt.Sprintf("共 %d 章没有取到，已跳过", failed))
	}
	return res, nil
}
