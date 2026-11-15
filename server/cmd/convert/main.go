// convert 是一个独立的小工具：把 EPUB 转成 EBF 目录。
//
// 用法：
//
//	go run ./cmd/convert -o ../out/书名.ebf "C:\path\book.epub"
//
// 服务端在「上传 EPUB」时会走同一个 ebf.Convert，所以这里的产物和服务端一致。
package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"clockreader-server/ebf"
)

func main() {
	indent := flag.Bool("indent", true, "段首两个全角空格")
	blank := flag.Bool("blank", false, "段间空行（默认关：小屏阅读器上太占地方）")
	skip := flag.Int("skip", 0, "跳过前 N 章（丢掉简介/前言这类开头页）")
	dedup := flag.Bool("dedup", true, "去掉正文开头与章名重复的那一行")
	out := flag.String("o", "", "输出目录；默认 <EPUB 同级>/<文件名>.ebf")
	flag.Parse()

	if flag.NArg() < 1 {
		fmt.Fprintln(os.Stderr, "用法: convert [选项] book.epub")
		flag.PrintDefaults()
		os.Exit(2)
	}

	src := flag.Arg(0)
	outDir := *out
	if outDir == "" {
		base := strings.TrimSuffix(filepath.Base(src), filepath.Ext(src))
		outDir = filepath.Join(filepath.Dir(src), base+".ebf")
	}

	res, err := ebf.Convert(src, outDir, ebf.Options{
		BlankLine:  *blank,
		Indent:     *indent,
		Skip:       *skip,
		DedupTitle: *dedup,
	})
	if err != nil {
		fmt.Fprintf(os.Stderr, "[失败] %s: %v\n", src, err)
		os.Exit(1)
	}

	mode := "按文件切分"
	if res.UsedTOC {
		mode = "按目录切分"
	}
	fmt.Printf("书名: %s\n作者: %s\n章节: %d（%s）\n正文: %d 字节\n输出: %s\n",
		res.Title, res.Author, res.Chapters, mode, res.Bytes, res.Dir)
	if res.Cover != nil {
		fmt.Printf("封面: %dx%d\n", res.Cover.W, res.Cover.H)
	}
	for _, w := range res.Warnings {
		fmt.Println("[警告]", w)
	}
}
