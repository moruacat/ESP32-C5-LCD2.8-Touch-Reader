// Package api 提供设备端使用的只读接口。
//
// 按「EBF 文件粒度」下发：设备拿到的就是 chapter.N.ebfl / data.ebfl 这些原始文件，
// 直接写 SD 卡即可，不需要解压、解析或重排——这是设备内存最省的做法。
package api

import (
	"encoding/json"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"clockreader-server/db"
)

const apiVersion = 1

// Handler 持有数据层。
type Handler struct {
	Store   *db.Store
	EbfRoot string
}

// Register 把设备 API 挂到 mux 上。
func (h *Handler) Register(mux *http.ServeMux) {
	mux.HandleFunc("/api/ping", h.ping)
	mux.HandleFunc("/api/books", h.books)
	mux.HandleFunc("/api/books/", h.book)
}

func writeJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	_ = json.NewEncoder(w).Encode(v)
}

func (h *Handler) ping(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, map[string]any{"ok": true, "ver": apiVersion})
}

// books 返回书单（设备用来对比本地有哪些书）。
func (h *Handler) books(w http.ResponseWriter, r *http.Request) {
	books, err := h.Store.ListBooks()
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, map[string]any{"ok": true, "books": books})
}

// book 处理 /api/books/{id}/{op}：
//
//	meta      → 章节数 + 内容哈希（设备据此判断要不要更新）
//	data      → data.ebfl
//	chapters  → chapters.ebfl（大纲）
//	cover     → cover.raw（RGB565 LE）
//	chapter/N → 第 N 章正文
func (h *Handler) book(w http.ResponseWriter, r *http.Request) {
	rest := strings.TrimPrefix(r.URL.Path, "/api/books/")
	if rest == "" {
		http.NotFound(w, r)
		return
	}

	parts := strings.SplitN(rest, "/", 2)
	id, err := strconv.ParseInt(parts[0], 10, 64)
	if err != nil {
		http.Error(w, "bad book id", http.StatusBadRequest)
		return
	}

	book, err := h.Store.GetBook(id)
	if err != nil {
		http.Error(w, "book not found", http.StatusNotFound)
		return
	}
	if book.EbfDir == "" {
		http.Error(w, "book has no ebf dir", http.StatusInternalServerError)
		return
	}

	op := ""
	if len(parts) == 2 {
		op = parts[1]
	}

	switch {
	case op == "meta":
		writeJSON(w, map[string]any{
			"ok": true, "id": book.ID, "title": book.Title, "author": book.Author,
			"chapters": book.Chapters, "bytes": book.Bytes, "hash": book.Hash,
			"cover_bytes": book.CoverBytes, "updated_at": book.UpdatedAt,
		})
	case op == "data":
		h.serveFile(w, book.EbfDir, "data.ebfl")
	case op == "chapters":
		h.serveFile(w, book.EbfDir, "chapters.ebfl")
	case op == "cover":
		h.serveFile(w, book.EbfDir, "cover.raw")
	case strings.HasPrefix(op, "chapter/"):
		n, err := strconv.Atoi(strings.TrimPrefix(op, "chapter/"))
		if err != nil || n < 1 {
			http.Error(w, "bad chapter number", http.StatusBadRequest)
			return
		}
		h.serveFile(w, book.EbfDir, "chapter."+strconv.Itoa(n)+".ebfl")
	default:
		http.NotFound(w, r)
	}
}

// serveFile 以流式方式回一个 EBF 文件（不整文件读进内存）。
func (h *Handler) serveFile(w http.ResponseWriter, dir, name string) {
	path := filepath.Join(dir, filepath.Clean(name))
	if !strings.HasPrefix(path, filepath.Clean(dir)) {
		http.Error(w, "bad path", http.StatusBadRequest)
		return
	}

	f, err := os.Open(path)
	if err != nil {
		http.NotFound(w, nil)
		return
	}
	defer f.Close()

	fi, err := f.Stat()
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}

	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Length", strconv.FormatInt(fi.Size(), 10))
	// io.Copy 自带 32KB 缓冲，按块流式发送，不会把整章读进内存
	_, _ = io.Copy(w, f)
}
