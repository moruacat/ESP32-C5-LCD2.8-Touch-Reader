// Package importer 负责把一次上传变成「EBF 目录 + 数据库记录」。
//
// 流程：临时 EPUB → ebf.Convert 到临时目录 → 按书名挪到 data/books/<书名>.ebf
// → 入库（同一本书复用 ID，实现覆盖更新）。
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
)

// Importer 把上传的 EPUB 转成书库条目。
type Importer struct {
	Store   *db.Store
	EbfRoot string // data/books
}

// DefaultOptions 是服务端的固定转换参数：按用户要求的观感——
// 段首两个全角空格 + 段间空行，并去掉正文开头重复的章名。
func DefaultOptions() ebf.Options {
	return ebf.Options{
		BlankLine:  true,
		Indent:     true,
		DedupTitle: true,
	}
}

// Import 处理一次上传。name 只用于日志；书名以 EPUB 里的元数据为准。
func (im *Importer) Import(name string, data []byte) (*db.Book, error) {
	if im.EbfRoot == "" {
		return nil, fmt.Errorf("未配置 EbfRoot")
	}
	if err := os.MkdirAll(im.EbfRoot, 0o755); err != nil {
		return nil, fmt.Errorf("创建书库目录失败: %w", err)
	}

	// 1. EPUB 落临时文件（ebf 包按路径打开 zip）
	tmpEpub, err := os.CreateTemp("", "upload-*.epub")
	if err != nil {
		return nil, fmt.Errorf("创建临时文件失败: %w", err)
	}
	tmpPath := tmpEpub.Name()
	defer os.Remove(tmpPath)
	if _, err := tmpEpub.Write(data); err != nil {
		tmpEpub.Close()
		return nil, fmt.Errorf("写临时文件失败: %w", err)
	}
	if err := tmpEpub.Close(); err != nil {
		return nil, err
	}

	// 2. 先转到临时目录：书名要解析完才知道，不能直接写最终位置。
	//    临时目录放在书库根下（同一分区），这样最后一步是原子 rename，
	//    跨盘时 os.Rename 会失败（Windows 的 D: → C: 就是典型）。
	tmpOut, err := os.MkdirTemp(im.EbfRoot, ".tmp-*")
	if err != nil {
		return nil, fmt.Errorf("创建临时输出目录失败: %w", err)
	}
	defer os.RemoveAll(tmpOut)

	res, err := ebf.Convert(tmpPath, tmpOut, DefaultOptions())
	if err != nil {
		return nil, fmt.Errorf("转换失败：%w", err)
	}

	log.Printf("转换完成：%q / %q  %d 章  %d 字节  %s  %s",
		res.Title, res.Author, res.Chapters, res.Bytes,
		func() string {
			if res.UsedTOC {
				return "按目录切分"
			}
			return "按文件切分"
		}(),
		func() string {
			if res.Cover != nil {
				return fmt.Sprintf("封面 %dx%d", res.Cover.W, res.Cover.H)
			}
			return "无封面"
		}())
	for _, warn := range res.Warnings {
		log.Printf("  [警告] %s", warn)
	}

	// 3. 按书名挪到最终位置（同名即覆盖更新）
	dirName := ebf.SafeDirName(res.Title) + ebf.EbfDirSuffix
	finalDir := filepath.Join(im.EbfRoot, dirName)
	if err := os.RemoveAll(finalDir); err != nil {
		return nil, fmt.Errorf("清理旧版本失败: %w", err)
	}
	if err := moveDir(tmpOut, finalDir); err != nil {
		return nil, fmt.Errorf("移动到书库失败: %w", err)
	}

	// 4. 入库：同书名+作者视为同一本书，复用 ID（覆盖更新）
	title, author := res.Title, res.Author
	existing, _ := im.Store.FindBook(title, author)

	// 整书内容哈希：把各章哈希再哈希一次，设备端对比 meta 的 hash 就知道
	// 「这本书有没有更新」，不用逐章请求。
	total := fnv.New64a()
	for _, c := range res.ChapterList {
		_, _ = total.Write([]byte(c.Hash))
	}

	book := &db.Book{
		Title:      title,
		Author:     author,
		Chapters:   res.Chapters,
		Bytes:      res.Bytes,
		Hash:       fmt.Sprintf("%x", total.Sum64()),
		Source:     "upload",
		EbfDir:     finalDir,
		CoverBytes: coverLen(res.Cover),
		UpdatedAt:  time.Now().Format("2006-01-02 15:04:05"),
	}
	if existing != nil {
		book.ID = existing.ID
		// 已有 fanqie_id 时保留，避免覆盖更新把追更绑定弄丢
		book.FanqieID = existing.FanqieID
	}

	id, err := im.Store.UpsertBook(book)
	if err != nil {
		return nil, fmt.Errorf("入库失败: %w", err)
	}
	book.ID = id

	// 5. 章节索引（供覆盖更新对比）
	list := make([]db.Chapter, 0, len(res.ChapterList))
	for _, c := range res.ChapterList {
		list = append(list, db.Chapter{
			BookID: id, No: c.No, Name: c.Title, Bytes: int64(c.Bytes), Hash: c.Hash,
		})
	}
	if err := im.Store.ReplaceChapters(id, list); err != nil {
		return nil, fmt.Errorf("写章节索引失败: %w", err)
	}

	return book, nil
}

func coverLen(c *ebf.Cover) int {
	if c == nil {
		return 0
	}
	return len(c.Data)
}

// moveDir 把目录挪到目标位置；rename 失败（跨分区）时退回「复制 + 删除」。
func moveDir(src, dst string) error {
	if err := os.Rename(src, dst); err == nil {
		return nil
	}
	if err := copyTree(src, dst); err != nil {
		return err
	}
	return os.RemoveAll(src)
}

// copyTree 递归复制目录。
func copyTree(src, dst string) error {
	return filepath.Walk(src, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel(src, path)
		if err != nil {
			return err
		}
		target := filepath.Join(dst, rel)

		if info.IsDir() {
			return os.MkdirAll(target, 0o755)
		}
		data, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return err
		}
		return os.WriteFile(target, data, 0o644)
	})
}
