// Package db 用纯 Go 的 modernc.org/sqlite 实现书库持久化。
//
// 选纯 Go 驱动是为了 CGO_ENABLED=0 交叉编译 i386——用户那台是 Intel Atom，
// 装 i686 交叉工具链只为编译一个 SQLite 不值得。
package db

import (
	"database/sql"
	"fmt"
	"strings"
	"time"

	_ "modernc.org/sqlite"
)

// Book 是一本书的元数据；EBF 文件本身存在 EbfDir 指向的目录里。
type Book struct {
	ID         int64  `json:"id"`
	Title      string `json:"title"`
	Author     string `json:"author"`
	FanqieID   string `json:"fanqie_id,omitempty"`
	Chapters   int    `json:"chapters"`
	Bytes      int64  `json:"bytes"`
	Hash       string `json:"hash"`
	Source     string `json:"source"` // upload / fanqie
	EbfDir     string `json:"-"`
	CoverBytes int    `json:"-"`
	UpdatedAt  string `json:"updated_at"`
}

// Chapter 用于覆盖更新对比：按 (book_id, no) 存内容哈希。
type Chapter struct {
	BookID int64
	No     int
	Name   string
	Bytes  int64
	Hash   string
}

// Store 包装一个 SQLite 连接池。
type Store struct {
	db *sql.DB
}

const schema = `
CREATE TABLE IF NOT EXISTS books (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    title       TEXT NOT NULL,
    author      TEXT DEFAULT '',
    fanqie_id   TEXT DEFAULT '',
    chapters    INTEGER DEFAULT 0,
    bytes       INTEGER DEFAULT 0,
    hash        TEXT DEFAULT '',
    source      TEXT DEFAULT 'upload',
    ebf_dir     TEXT NOT NULL,
    cover_bytes INTEGER DEFAULT 0,
    updated_at  TEXT DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_books_title ON books(title, author);

CREATE TABLE IF NOT EXISTS chapters (
    book_id INTEGER NOT NULL,
    no      INTEGER NOT NULL,
    name    TEXT DEFAULT '',
    bytes   INTEGER DEFAULT 0,
    hash    TEXT DEFAULT '',
    PRIMARY KEY (book_id, no)
);

CREATE TABLE IF NOT EXISTS jobs (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    kind       TEXT NOT NULL,
    status     TEXT NOT NULL DEFAULT 'pending',
    progress   INTEGER DEFAULT 0,
    message    TEXT DEFAULT '',
    book_id    INTEGER DEFAULT 0,
    created_at TEXT DEFAULT ''
);
`

// Open 打开（必要时创建）数据库并建表。
func Open(path string) (*Store, error) {
	// WAL + busy_timeout：Web 请求和追更任务会并发读写
	dsn := path + "?_pragma=journal_mode(WAL)&_pragma=busy_timeout(5000)"
	conn, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, fmt.Errorf("打开数据库失败: %w", err)
	}
	conn.SetMaxOpenConns(4)
	if _, err := conn.Exec(schema); err != nil {
		conn.Close()
		return nil, fmt.Errorf("建表失败: %w", err)
	}
	return &Store{db: conn}, nil
}

func (s *Store) Close() error { return s.db.Close() }

// ListBooks 返回全部书，最近的在前。
func (s *Store) ListBooks() ([]Book, error) {
	rows, err := s.db.Query(`
		SELECT id, title, author, fanqie_id, chapters, bytes, hash, source,
		       ebf_dir, cover_bytes, updated_at
		FROM books ORDER BY updated_at DESC, id DESC`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var out []Book
	for rows.Next() {
		var b Book
		if err := rows.Scan(&b.ID, &b.Title, &b.Author, &b.FanqieID, &b.Chapters,
			&b.Bytes, &b.Hash, &b.Source, &b.EbfDir, &b.CoverBytes, &b.UpdatedAt); err != nil {
			return nil, err
		}
		out = append(out, b)
	}
	return out, rows.Err()
}

// GetBook 按 ID 取一本书。
func (s *Store) GetBook(id int64) (*Book, error) {
	var b Book
	err := s.db.QueryRow(`
		SELECT id, title, author, fanqie_id, chapters, bytes, hash, source,
		       ebf_dir, cover_bytes, updated_at
		FROM books WHERE id = ?`, id).
		Scan(&b.ID, &b.Title, &b.Author, &b.FanqieID, &b.Chapters,
			&b.Bytes, &b.Hash, &b.Source, &b.EbfDir, &b.CoverBytes, &b.UpdatedAt)
	if err != nil {
		return nil, err
	}
	return &b, nil
}

// FindBook 按书名+作者查（覆盖更新用）。
func (s *Store) FindBook(title, author string) (*Book, error) {
	var b Book
	err := s.db.QueryRow(`
		SELECT id, title, author, fanqie_id, chapters, bytes, hash, source,
		       ebf_dir, cover_bytes, updated_at
		FROM books WHERE title = ? AND author = ?`, title, author).
		Scan(&b.ID, &b.Title, &b.Author, &b.FanqieID, &b.Chapters,
			&b.Bytes, &b.Hash, &b.Source, &b.EbfDir, &b.CoverBytes, &b.UpdatedAt)
	if err != nil {
		return nil, err
	}
	return &b, nil
}

// UpsertBook 插入或更新一本书，返回 ID。
func (s *Store) UpsertBook(b *Book) (int64, error) {
	now := time.Now().Format("2006-01-02 15:04:05")
	if b.UpdatedAt == "" {
		b.UpdatedAt = now
	}

	if b.ID > 0 {
		_, err := s.db.Exec(`
			UPDATE books SET title=?, author=?, fanqie_id=?, chapters=?, bytes=?,
			       hash=?, source=?, ebf_dir=?, cover_bytes=?, updated_at=?
			WHERE id=?`,
			b.Title, b.Author, b.FanqieID, b.Chapters, b.Bytes,
			b.Hash, b.Source, b.EbfDir, b.CoverBytes, b.UpdatedAt, b.ID)
		return b.ID, err
	}

	res, err := s.db.Exec(`
		INSERT INTO books (title, author, fanqie_id, chapters, bytes, hash,
		                   source, ebf_dir, cover_bytes, updated_at)
		VALUES (?,?,?,?,?,?,?,?,?,?)`,
		b.Title, b.Author, b.FanqieID, b.Chapters, b.Bytes,
		b.Hash, b.Source, b.EbfDir, b.CoverBytes, b.UpdatedAt)
	if err != nil {
		return 0, err
	}
	return res.LastInsertId()
}

// ReplaceChapters 用一个事务替换某本书的全部章节索引（转换完成后调用）。
func (s *Store) ReplaceChapters(bookID int64, list []Chapter) error {
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()

	if _, err := tx.Exec(`DELETE FROM chapters WHERE book_id=?`, bookID); err != nil {
		return err
	}
	stmt, err := tx.Prepare(`INSERT INTO chapters (book_id, no, name, bytes, hash) VALUES (?,?,?,?,?)`)
	if err != nil {
		return err
	}
	defer stmt.Close()
	for _, c := range list {
		if _, err := stmt.Exec(bookID, c.No, c.Name, c.Bytes, c.Hash); err != nil {
			return err
		}
	}
	return tx.Commit()
}

// ListChapters 取某本书的章节索引。
func (s *Store) ListChapters(bookID int64) ([]Chapter, error) {
	rows, err := s.db.Query(`
		SELECT book_id, no, name, bytes, hash FROM chapters
		WHERE book_id=? ORDER BY no`, bookID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var out []Chapter
	for rows.Next() {
		var c Chapter
		if err := rows.Scan(&c.BookID, &c.No, &c.Name, &c.Bytes, &c.Hash); err != nil {
			return nil, err
		}
		out = append(out, c)
	}
	return out, rows.Err()
}

// DeleteBook 删除一本书及其章节索引。
func (s *Store) DeleteBook(id int64) error {
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if _, err := tx.Exec(`DELETE FROM chapters WHERE book_id=?`, id); err != nil {
		return err
	}
	if _, err := tx.Exec(`DELETE FROM books WHERE id=?`, id); err != nil {
		return err
	}
	return tx.Commit()
}

// ListOptions 是管理页书库列表的查询条件。
type ListOptions struct {
	Search string // 书名/作者模糊匹配
	Source string // "upload" / "fanqie"；空 = 全部
	Sort   string // "recent"（默认）/ "title"
	Offset int
	Limit  int
}

// ListBooksPage 按条件分页查书，同时返回满足条件的总数。
func (s *Store) ListBooksPage(opt ListOptions) ([]Book, int, error) {
	var where []string
	var args []any

	if opt.Search != "" {
		where = append(where, "(title LIKE ? OR author LIKE ?)")
		like := "%" + opt.Search + "%"
		args = append(args, like, like)
	}
	if opt.Source != "" {
		where = append(where, "source = ?")
		args = append(args, opt.Source)
	}

	cond := ""
	if len(where) > 0 {
		cond = " WHERE " + strings.Join(where, " AND ")
	}

	var total int
	if err := s.db.QueryRow("SELECT COUNT(*) FROM books"+cond, args...).Scan(&total); err != nil {
		return nil, 0, err
	}

	order := " ORDER BY updated_at DESC, id DESC"
	if opt.Sort == "title" {
		order = " ORDER BY title ASC"
	}

	limit := opt.Limit
	if limit <= 0 {
		limit = 24
	}
	if opt.Offset < 0 {
		opt.Offset = 0
	}

	q := `SELECT id, title, author, fanqie_id, chapters, bytes, hash, source,
	             ebf_dir, cover_bytes, updated_at
	      FROM books` + cond + order + ` LIMIT ? OFFSET ?`
	args = append(args, limit, opt.Offset)

	rows, err := s.db.Query(q, args...)
	if err != nil {
		return nil, 0, err
	}
	defer rows.Close()

	var out []Book
	for rows.Next() {
		var b Book
		if err := rows.Scan(&b.ID, &b.Title, &b.Author, &b.FanqieID, &b.Chapters,
			&b.Bytes, &b.Hash, &b.Source, &b.EbfDir, &b.CoverBytes, &b.UpdatedAt); err != nil {
			return nil, 0, err
		}
		out = append(out, b)
	}
	return out, total, rows.Err()
}

// Stats 返回全库概览数字。
func (s *Store) Stats() (books int, chapters int, bytes int64, err error) {
	err = s.db.QueryRow(`
		SELECT COUNT(*), COALESCE(SUM(chapters), 0), COALESCE(SUM(bytes), 0)
		FROM books`).Scan(&books, &chapters, &bytes)
	return
}

// Job 是一条后台任务记录。
type Job struct {
	ID        int64
	Kind      string
	Status    string
	Progress  int
	Message   string
	BookID    int64
	CreatedAt string
}

// RecentJobs 取最近的任务，新的在前。
func (s *Store) RecentJobs(limit int) ([]Job, error) {
	if limit <= 0 {
		limit = 5
	}
	rows, err := s.db.Query(`
		SELECT id, kind, status, progress, message, book_id, created_at
		FROM jobs ORDER BY id DESC LIMIT ?`, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var out []Job
	for rows.Next() {
		var j Job
		if err := rows.Scan(&j.ID, &j.Kind, &j.Status, &j.Progress,
			&j.Message, &j.BookID, &j.CreatedAt); err != nil {
			return nil, err
		}
		out = append(out, j)
	}
	return out, rows.Err()
}

// ListFanqieBooks 取出所有番茄来源的书（追更用）。
func (s *Store) ListFanqieBooks() ([]Book, error) {
	rows, err := s.db.Query(`
		SELECT id, title, author, fanqie_id, chapters, bytes, hash, source,
		       ebf_dir, cover_bytes, updated_at
		FROM books WHERE source = 'fanqie' AND fanqie_id != '' ORDER BY id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var out []Book
	for rows.Next() {
		var b Book
		if err := rows.Scan(&b.ID, &b.Title, &b.Author, &b.FanqieID, &b.Chapters,
			&b.Bytes, &b.Hash, &b.Source, &b.EbfDir, &b.CoverBytes, &b.UpdatedAt); err != nil {
			return nil, err
		}
		out = append(out, b)
	}
	return out, rows.Err()
}

// AddJob 记录一个后台任务，返回 ID。
func (s *Store) AddJob(kind string) (int64, error) {
	res, err := s.db.Exec(`
		INSERT INTO jobs (kind, status, created_at) VALUES (?, 'running', ?)`,
		kind, time.Now().Format("2006-01-02 15:04:05"))
	if err != nil {
		return 0, err
	}
	return res.LastInsertId()
}

// UpdateJob 更新任务进度/结果。
func (s *Store) UpdateJob(id int64, status string, progress int, message string, bookID int64) error {
	_, err := s.db.Exec(`
		UPDATE jobs SET status=?, progress=?, message=?, book_id=? WHERE id=?`,
		status, progress, message, bookID, id)
	return err
}
