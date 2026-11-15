package importer

import (
	"fmt"
	"hash/fnv"
	"log"
	"os"
	"path/filepath"
	"time"

	"clockreader-server/db"
	"clockreader-server/ebf"
	"clockreader-server/fanqie"
)

// FanqieOptions 是番茄导入/追更的固定参数。
//
// Delay 200ms：番茄对正文请求有频控，太快会触发验证码。
func FanqieOptions() fanqie.ImportOptions {
	return fanqie.ImportOptions{Delay: 200 * time.Millisecond}
}

// ImportFanqie 抓一本番茄书并入库。
func (im *Importer) ImportFanqie(book fanqie.Book, opts fanqie.ImportOptions) (*db.Book, error) {
	jobID, _ := im.Store.AddJob("fanqie")
	if jobID > 0 {
		_ = im.Store.UpdateJob(jobID, "running", 5,
			fmt.Sprintf("番茄《%s》抓取中…", book.Title), 0)
	}

	saved, err := im.importFanqieInternal(book, opts)
	if err != nil {
		if jobID > 0 {
			_ = im.Store.UpdateJob(jobID, "fail", 0,
				fmt.Sprintf("番茄《%s》失败：%v", book.Title, err), 0)
		}
		return nil, err
	}

	if jobID > 0 {
		_ = im.Store.UpdateJob(jobID, "done", 100,
			fmt.Sprintf("番茄《%s》完成 · %d 章", saved.Title, saved.Chapters), saved.ID)
	}
	return saved, nil
}

func (im *Importer) importFanqieInternal(book fanqie.Book, opts fanqie.ImportOptions) (*db.Book, error) {
	if im.EbfRoot == "" {
		return nil, fmt.Errorf("未配置 EbfRoot")
	}
	if err := os.MkdirAll(im.EbfRoot, 0o755); err != nil {
		return nil, fmt.Errorf("创建书库目录失败: %w", err)
	}

	// 先抓到临时目录（同分区，最后一步原子 rename）
	tmpOut, err := os.MkdirTemp(im.EbfRoot, ".tmp-*")
	if err != nil {
		return nil, fmt.Errorf("创建临时目录失败: %w", err)
	}
	defer os.RemoveAll(tmpOut)

	cli := fanqie.NewClient()
	res, err := fanqie.Import(cli, book, tmpOut, opts)
	if err != nil {
		return nil, err
	}
	for _, w := range res.Warnings {
		log.Printf("番茄《%s》警告：%s", res.Title, w)
	}

	dirName := ebf.SafeDirName(res.Title) + ebf.EbfDirSuffix
	finalDir := filepath.Join(im.EbfRoot, dirName)
	if err := os.RemoveAll(finalDir); err != nil {
		return nil, fmt.Errorf("清理旧版本失败: %w", err)
	}
	if err := moveDir(tmpOut, finalDir); err != nil {
		return nil, fmt.Errorf("移动到书库失败: %w", err)
	}

	return im.saveFanqieBook(book, res, finalDir)
}

// saveFanqieBook 把转换结果写进数据库（同书名+作者复用 ID）。
func (im *Importer) saveFanqieBook(src fanqie.Book, res *ebf.Result, dir string) (*db.Book, error) {
	existing, _ := im.Store.FindBook(res.Title, res.Author)

	total := fnv.New64a()
	chapters := make([]db.Chapter, 0, len(res.ChapterList))
	for _, c := range res.ChapterList {
		_, _ = total.Write([]byte(c.Hash))
		chapters = append(chapters, db.Chapter{
			No: c.No, Name: c.Title, Bytes: int64(c.Bytes), Hash: c.Hash,
		})
	}

	book := &db.Book{
		Title:      res.Title,
		Author:     res.Author,
		FanqieID:   src.BookID,
		Chapters:   res.Chapters,
		Bytes:      res.Bytes,
		Hash:       fmt.Sprintf("%x", total.Sum64()),
		Source:     "fanqie",
		EbfDir:     dir,
		CoverBytes: coverLen(res.Cover),
		UpdatedAt:  time.Now().Format("2006-01-02 15:04:05"),
	}
	if existing != nil {
		book.ID = existing.ID
	}

	id, err := im.Store.UpsertBook(book)
	if err != nil {
		return nil, fmt.Errorf("入库失败: %w", err)
	}
	book.ID = id

	for i := range chapters {
		chapters[i].BookID = id
	}
	if err := im.Store.ReplaceChapters(id, chapters); err != nil {
		return nil, fmt.Errorf("写章节索引失败: %w", err)
	}
	return book, nil
}

// UpdateFanqie 检查一本已导入的番茄书，有新章就增量补上。
// 返回 (是否有更新, 新增章数, 错误)。
func (im *Importer) UpdateFanqie(book *db.Book, opts fanqie.ImportOptions) (bool, int, error) {
	if book.FanqieID == "" || book.EbfDir == "" {
		return false, 0, fmt.Errorf("不是番茄书或缺少下载目录")
	}

	cli := fanqie.NewClient()
	info, err := cli.BookInfo(book.FanqieID)
	if err != nil {
		return false, 0, err
	}
	// 详情页拿到的书名/作者更准，但为空时沿用库里的
	if info.Title == "" {
		info.Title = book.Title
	}
	if info.Author == "" {
		info.Author = book.Author
	}

	before := book.Chapters
	res, updated, err := fanqie.Update(cli, *info, book.EbfDir, book.Chapters, opts)
	if err != nil || !updated {
		return false, 0, err
	}

	// 追更后重新统计目录大小（原文的 Bytes 只算了新增部分）
	size := dirSize(book.EbfDir)

	book.Chapters = res.Chapters
	book.Bytes = size
	book.CoverBytes = coverLen(res.Cover)
	book.UpdatedAt = time.Now().Format("2006-01-02 15:04:05")

	if _, err := im.Store.UpsertBook(book); err != nil {
		return false, 0, err
	}

	list := make([]db.Chapter, 0, len(res.ChapterList))
	for _, c := range res.ChapterList {
		list = append(list, db.Chapter{
			BookID: book.ID, No: c.No, Name: c.Title,
			Bytes: int64(c.Bytes), Hash: c.Hash,
		})
	}
	if err := im.Store.ReplaceChapters(book.ID, list); err != nil {
		return false, 0, err
	}

	return true, res.Chapters - before, nil
}

// dirSize 统计目录下所有文件的字节数。
func dirSize(dir string) int64 {
	var total int64
	_ = filepath.Walk(dir, func(_ string, info os.FileInfo, err error) error {
		if err == nil && info != nil && !info.IsDir() {
			total += info.Size()
		}
		return nil
	})
	return total
}
