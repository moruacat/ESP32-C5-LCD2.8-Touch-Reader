// Package web 提供管理界面（书单、上传、任务）。
package web

import (
	"fmt"
	"html/template"
	"io"
	"log"
	"net/http"

	"clockreader-server/db"
)

// Handler 持有数据层与上传依赖。
type Handler struct {
	Store *db.Store
	// Importer 在 EPUB 上传后被调用（转换 + 入库）。
	// 目前为 nil：转换器尚未接入时上传会明确报错，而不是假成功后无声失败。
	Importer func(name string, data []byte) (*db.Book, error)
}

var indexTmpl = template.Must(template.New("index").Parse(`<!doctype html>
<html lang="zh"><head><meta charset="utf-8">
<title>ClockReader 书房</title>
<style>
 body{font-family:system-ui,sans-serif;background:#0d1117;color:#e6e9ef;margin:0;padding:24px}
 h1{font-size:20px;margin:0 0 16px}
 .muted{color:#8b949e;font-size:13px}
 table{border-collapse:collapse;width:100%;margin-top:16px}
 th,td{text-align:left;padding:8px 10px;border-bottom:1px solid #21262d;font-size:14px}
 th{color:#8b949e;font-weight:500}
 .tag{display:inline-block;padding:1px 6px;border-radius:4px;background:#1585c0;color:#fff;font-size:12px}
 .empty{padding:40px 0;text-align:center;color:#8b949e}
 form{margin-top:18px;padding:14px;border:1px dashed #30363d;border-radius:8px}
 input[type=file]{color:#e6e9ef}
 button{margin-top:8px;padding:6px 14px;border:0;border-radius:6px;background:#1585c0;color:#fff;font-size:14px}
 .err{color:#e5484d;margin-top:8px;font-size:13px}
</style></head><body>
<h1>ClockReader 书房</h1>
<p class="muted">管理页 · 设备通过 http://&lt;本机 IP&gt;:{{.Addr}} 的 /api/ 接口拉取书库</p>

{{if .Err}}<p class="err">{{.Err}}</p>{{end}}

<form method="post" action="/upload" enctype="multipart/form-data">
  <div class="muted">上传 EPUB（文件名尽量写成「书名 - 作者.epub」，转换器会优先用它判断是否覆盖已有书）</div>
  <input type="file" name="file" accept=".epub" required>
  <button type="submit">上传并转换</button>
</form>

{{if .Books}}
<table>
<thead><tr><th>书名</th><th>作者</th><th>章节</th><th>大小</th><th>来源</th><th>更新时间</th></tr></thead>
<tbody>
{{range .Books}}
<tr>
  <td>{{.Title}}</td>
  <td>{{.Author}}</td>
  <td>{{.Chapters}}</td>
  <td>{{.BytesHuman}}</td>
  <td><span class="tag">{{.Source}}</span></td>
  <td class="muted">{{.UpdatedAt}}</td>
</tr>
{{end}}
</tbody></table>
{{else}}
<div class="empty">书库还是空的。上传一本 EPUB 试试。</div>
{{end}}

</body></html>`))

// BookRow 是模板用的行视图。
type BookRow struct {
	Title, Author, Source, UpdatedAt string
	Chapters                         int
	BytesHuman                       string
}

func humanBytes(n int64) string {
	const unit = 1024
	if n < unit {
		return fmt.Sprintf("%d B", n)
	}
	div, exp := int64(unit), 0
	for m := n / unit; m >= unit; m /= unit {
		div *= unit
		exp++
	}
	return fmt.Sprintf("%.1f %cB", float64(n)/float64(div), "KMGT"[exp])
}

// Register 挂管理页路由。
func (h *Handler) Register(mux *http.ServeMux, addr string) {
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/" {
			http.NotFound(w, r)
			return
		}
		var errMsg string
		books, err := h.Store.ListBooks()
		if err != nil {
			errMsg = "读取书库失败: " + err.Error()
		}
		rows := make([]BookRow, 0, len(books))
		for _, b := range books {
			rows = append(rows, BookRow{
				Title: b.Title, Author: b.Author, Source: b.Source,
				UpdatedAt: b.UpdatedAt, Chapters: b.Chapters,
				BytesHuman: humanBytes(b.Bytes),
			})
		}
		data := struct {
			Addr  string
			Books []BookRow
			Err   string
		}{Addr: addr, Books: rows, Err: errMsg}
		if err := indexTmpl.Execute(w, data); err != nil {
			log.Printf("渲染管理页失败: %v", err)
		}
	})

	mux.HandleFunc("/upload", h.upload)
}

// upload 接收 EPUB 并交给 Importer 转换入库。
func (h *Handler) upload(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "只支持 POST", http.StatusMethodNotAllowed)
		return
	}
	if h.Importer == nil {
		http.Error(w, "转换器尚未接入（阶段 B），上传暂不可用", http.StatusNotImplemented)
		return
	}

	// 200MB 上限：EPUB 一般只有几 MB，这个尺寸只用来挡住误传的大文件
	r.Body = http.MaxBytesReader(w, r.Body, 200<<20)
	if err := r.ParseMultipartForm(16 << 20); err != nil {
		http.Error(w, "解析上传失败（文件过大或不是 multipart）："+err.Error(),
			http.StatusBadRequest)
		return
	}
	if r.MultipartForm != nil {
		defer r.MultipartForm.RemoveAll()
	}

	file, hdr, err := r.FormFile("file")
	if err != nil {
		http.Error(w, "没有收到 file 字段："+err.Error(), http.StatusBadRequest)
		return
	}
	defer file.Close()

	data, err := io.ReadAll(file)
	if err != nil {
		http.Error(w, "读取上传内容失败："+err.Error(), http.StatusBadRequest)
		return
	}

	book, err := h.Importer(hdr.Filename, data)
	if err != nil {
		log.Printf("导入 %s 失败: %v", hdr.Filename, err)
		http.Error(w, "导入失败："+err.Error(), http.StatusInternalServerError)
		return
	}

	log.Printf("已导入 %q：%d 章，%d 字节", book.Title, book.Chapters, book.Bytes)
	http.Redirect(w, r, "/", http.StatusSeeOther)
}
