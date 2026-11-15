package fanqie

import (
	"fmt"
	"hash/fnv"
	"time"

	"clockreader-server/ebf"
)

// ImportOptions 控制一次番茄导入。
type ImportOptions struct {
	// Delay 每章之间的间隔。番茄对正文请求有频控，太快容易触发验证码。
	Delay time.Duration
	// MaxChapters 只抓前 N 章（0 = 全部）。调试/预览时用。
	MaxChapters int
	// Progress 进度回调（已完成, 总数, 当前章名）。
	Progress func(done, total int, title string)
}

// Import 抓一本番茄书并写成 EBF 目录。
//
// 锁定/付费章节会被跳过（不绕过站点的账号与版权限制），并作为警告返回。
func Import(cli *Client, book Book, outDir string, opt ImportOptions) (*ebf.Result, error) {
	dir, err := cli.Directory(book.BookID)
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
		if ch.Locked {
			failed++
			warnings = append(warnings, "跳过锁定章节："+ch.Title)
			continue
		}

		title, text, err := cli.FetchChapter(book.BookID, ch.ItemID)
		if err != nil {
			failed++
			warnings = append(warnings, fmt.Sprintf("第 %d 章失败：%v", i+1, err))
			continue
		}
		if title == "" {
			title = ch.Title
		}

		// 番茄没分卷的书会给一个「第一卷：默认」的占位卷名，
		// 原样带进去会让 data.ebfl 多出一张只有一卷的卷表。
		volume := ch.Volume
		if volume == "第一卷：默认" {
			volume = ""
		}

		// 番茄正文本身没有段首缩进/段间空行，直接一行一段落盘
		out = append(out, ebf.Chapter{
			Title:  title,
			Volume: volume,
			Text:   text + "\n",
		})

		if opt.Progress != nil {
			opt.Progress(i+1, total, title)
		}
		if opt.Delay > 0 {
			time.Sleep(opt.Delay)
		}
	}

	if len(out) == 0 {
		return nil, fmt.Errorf("没有任何可下载的章节（全部锁定或失败）")
	}

	intro := book.Abstract
	if len(intro) > ebf.IntroMaxBytes {
		intro = ebf.TruncateUTF8(intro, ebf.IntroMaxBytes)
	}

	if err := ebf.WriteDir(outDir, out, book.Title, book.Author, intro, nil); err != nil {
		return nil, err
	}

	res := &ebf.Result{
		Title:    book.Title,
		Author:   book.Author,
		Chapters: len(out),
		Dir:      outDir,
		Warnings: warnings,
	}
	for i, c := range out {
		res.Bytes += int64(len(c.Text))
		res.ChapterList = append(res.ChapterList, ebf.ChapterInfo{
			No:    i + 1,
			Title: c.Title,
			Bytes: len(c.Text),
			Hash:  contentHash(c.Text),
		})
	}
	if failed > 0 {
		res.Warnings = append(res.Warnings,
			fmt.Sprintf("共 %d 章没有取到（锁定或请求失败），已跳过", failed))
	}
	return res, nil
}

// contentHash 与 ebf 包内部用的算法一致（覆盖更新时对比章节用）。
func contentHash(s string) string {
	h := fnv.New64a()
	_, _ = h.Write([]byte(s))
	return fmt.Sprintf("%x", h.Sum64())
}
