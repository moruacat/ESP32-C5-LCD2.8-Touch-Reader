// fanqie 是番茄小说的命令行工具，用于手动搜索/导入与排查。
//
//	go run ./cmd/fanqie search "关键词"
//	go run ./cmd/fanqie import <bookId> -o ./out/书名.ebf [-n 5] [-delay 300]
package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"clockreader-server/fanqie"
)

func main() {
	if len(os.Args) < 2 {
		usage()
	}

	cmd := os.Args[1]
	args := os.Args[2:]

	switch cmd {
	case "search":
		cmdSearch(args)
	case "import":
		cmdImport(args)
	default:
		usage()
	}
}

func usage() {
	fmt.Fprintln(os.Stderr, `用法（选项要放在位置参数前面）:
  fanqie search -n 10 "关键词"
  fanqie import -o <输出目录> -n <章数> -delay 250 <bookId>`)
	os.Exit(2)
}

func cmdSearch(args []string) {
	fs := flag.NewFlagSet("search", flag.ExitOnError)
	limit := fs.Int("n", 10, "最多显示多少条")
	_ = fs.Parse(args)

	if fs.NArg() < 1 {
		usage()
	}
	keyword := fs.Arg(0)

	cli := fanqie.NewClient()
	results, more, err := cli.Search(keyword, 0)
	if err != nil {
		fmt.Fprintln(os.Stderr, "搜索失败:", err)
		os.Exit(1)
	}

	if len(results) == 0 {
		fmt.Println("没有结果")
		return
	}

	n := len(results)
	if *limit < n {
		n = *limit
	}
	for i := 0; i < n; i++ {
		r := results[i]
		fmt.Printf("%d) %s\n   作者: %s  评分: %s  bookId: %s\n", i+1, r.Title, r.Author, r.Score, r.BookID)
		if r.Abstract != "" {
			fmt.Printf("   %s\n", summary(r.Abstract, 70))
		}
	}
	if more {
		fmt.Println("（还有更多结果）")
	}
}

func cmdImport(args []string) {
	fs := flag.NewFlagSet("import", flag.ExitOnError)
	out := fs.String("o", "", "输出目录（.ebf）")
	maxChapters := fs.Int("n", 0, "只抓前 N 章（0 = 全部）")
	delayMS := fs.Int("delay", 250, "每章间隔毫秒")
	_ = fs.Parse(args)

	if fs.NArg() < 1 {
		usage()
	}
	bookID := fs.Arg(0)

	cli := fanqie.NewClient()

	// 从详情页取书名/作者；取不到就退回 bookId
	book := fanqie.Book{BookID: bookID, Title: bookID}
	if info, err := cli.BookInfo(bookID); err == nil {
		book = *info
	} else {
		fmt.Println("[警告] 详情页读取失败，书名暂用 bookId：", err)
	}

	outDir := *out
	if outDir == "" {
		outDir = filepath.Join(".", safeFileName(book.Title)+".ebf")
	}

	fmt.Printf("开始导入: %s / %s\n", book.Title, book.Author)
	start := time.Now()

	res, err := fanqie.Import(cli, book, outDir, fanqie.ImportOptions{
		Delay:       time.Duration(*delayMS) * time.Millisecond,
		MaxChapters: *maxChapters,
		Progress: func(done, total int, title string) {
			if done%10 == 0 || done == total {
				fmt.Printf("  [%d/%d] %s\n", done, total, summary(title, 40))
			}
		},
	})
	if err != nil {
		fmt.Fprintln(os.Stderr, "导入失败:", err)
		os.Exit(1)
	}

	fmt.Printf("完成: %d 章，%d 字节，用时 %s\n输出: %s\n",
		res.Chapters, res.Bytes, time.Since(start).Round(time.Second), res.Dir)
	for _, w := range res.Warnings {
		fmt.Println("[警告]", w)
	}
}

func summary(s string, n int) string {
	s = strings.ReplaceAll(s, "\n", " ")
	r := []rune(s)
	if len(r) <= n {
		return s
	}
	return string(r[:n]) + "…"
}

func safeFileName(s string) string {
	bad := []string{`\`, "/", ":", "*", "?", `"`, "<", ">", "|"}
	for _, b := range bad {
		s = strings.ReplaceAll(s, b, "_")
	}
	s = strings.TrimSpace(s)
	if s == "" {
		return "book"
	}
	return s
}
