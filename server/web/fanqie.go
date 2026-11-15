package web

import (
	"html/template"
	"log"
	"net/http"
	"strings"

	"clockreader-server/db"
	"clockreader-server/fanqie"
)

// registerFanqie 挂番茄搜索/导入/更新路由。
func (h *Handler) registerFanqie(mux *http.ServeMux) {
	mux.HandleFunc("/fanqie", h.fanqieSearch)
	mux.HandleFunc("/fanqie/import", h.fanqieImport)
	mux.HandleFunc("/fanqie/update", h.fanqieUpdate)
}

var fanqieTmpl = template.Must(template.New("fanqie").Parse(`<!doctype html>
<html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="dark">
<title>番茄搜索 · ClockReader 书房</title>
<style>
 *{box-sizing:border-box}
 body{margin:0;color:#e9eff7;font-size:14px;line-height:1.55;
  font-family:-apple-system,"Segoe UI","PingFang SC","Microsoft YaHei",system-ui,sans-serif;
  background:radial-gradient(900px 480px at 10% -12%,rgba(21,133,192,.20),transparent 62%),#080c12;
  background-attachment:fixed;-webkit-font-smoothing:antialiased}
 .wrap{max-width:900px;margin:0 auto;padding:26px 20px 48px}
 a{color:#6fc6f0;text-decoration:none}
 a:hover{text-decoration:underline}
 h1{font-size:19px;margin:0 0 4px}
 .muted{color:#8496a9;font-size:12.5px}
 .bar{display:flex;gap:8px;margin:18px 0;flex-wrap:wrap}
 input[type=search]{flex:1;min-width:200px;height:38px;border-radius:9px;border:1px solid #1d2937;
  background:#0b121a;color:#e9eff7;padding:0 12px;font-size:14px;outline:none}
 input[type=search]:focus{border-color:#1585c0}
 button{height:38px;padding:0 16px;border:0;border-radius:9px;cursor:pointer;
  font-size:14px;color:#fff;background:#1585c0}
 button:hover{background:#1ba3e6}
 .card{border:1px solid #1d2937;border-radius:13px;padding:14px 16px;margin-bottom:12px;
  background:linear-gradient(180deg,#111a24,#0d141d)}
 .row{display:flex;gap:12px;align-items:flex-start}
 .meta{flex:1;min-width:0}
 .title{font-weight:600}
 .abs{color:#8496a9;font-size:12.5px;margin-top:4px;
  display:-webkit-box;-webkit-line-clamp:2;-webkit-box-orient:vertical;overflow:hidden}
 .err{color:#e5484d;margin:10px 0}
 .empty{color:#8496a9;padding:30px 0;text-align:center}
 .tag{display:inline-block;padding:2px 8px;border-radius:6px;font-size:11.5px;
  background:rgba(229,72,77,.16);color:#f08589;margin-left:6px}
</style></head><body>
<div class="wrap">
  <h1>番茄小说</h1>
  <p class="muted">搜索后点「导入」会后台抓取并转成 EBF，进度看管理页的任务日志</p>

  <form class="bar" method="get" action="/fanqie">
    <input type="search" name="q" value="{{.Query}}" placeholder="输入书名或作者…" autofocus>
    <button type="submit">搜索</button>
    <a href="/" style="align-self:center;font-size:13px">返回书房</a>
  </form>

  {{if .Err}}<p class="err">{{.Err}}</p>{{end}}

  {{if .Results}}
    {{range .Results}}
    <div class="card">
      <div class="row">
        <div class="meta">
          <div class="title">{{.Title}}{{if .Score}}<span class="tag">评分 {{.Score}}</span>{{end}}</div>
          <div class="muted">{{if .Author}}{{.Author}}{{else}}—{{end}} · bookId {{.BookID}}</div>
          {{if .Abstract}}<div class="abs">{{.Abstract}}</div>{{end}}
        </div>
        <form method="post" action="/fanqie/import" style="margin:0">
          <input type="hidden" name="bookId" value="{{.BookID}}">
          <input type="hidden" name="title" value="{{.Title}}">
          <input type="hidden" name="author" value="{{.Author}}">
          <button type="submit">导入</button>
        </form>
      </div>
    </div>
    {{end}}
  {{else if .Query}}
    <div class="empty">没有搜到结果</div>
  {{else}}
    <div class="empty">输入关键词开始搜索</div>
  {{end}}
</div>
</body></html>`))

type fanqieItem struct {
	BookID   string
	Title    string
	Author   string
	Abstract string
	Score    string
}

func (h *Handler) fanqieSearch(w http.ResponseWriter, r *http.Request) {
	q := strings.TrimSpace(r.URL.Query().Get("q"))

	var items []fanqieItem
	var errMsg string

	if q != "" {
		cli := fanqie.NewClient()
		results, _, err := cli.Search(q, 0)
		if err != nil {
			errMsg = "搜索失败：" + err.Error()
		} else {
			for _, res := range results {
				items = append(items, fanqieItem{
					BookID: res.BookID, Title: res.Title, Author: res.Author,
					Abstract: res.Abstract, Score: res.Score,
				})
			}
		}
	}

	data := struct {
		Query   string
		Results []fanqieItem
		Err     string
	}{Query: q, Results: items, Err: errMsg}

	if err := fanqieTmpl.Execute(w, data); err != nil {
		log.Printf("渲染番茄搜索页失败: %v", err)
	}
}

// fanqieImport 后台导入：几百章要几分钟，不能卡住 HTTP 请求。
func (h *Handler) fanqieImport(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "只支持 POST", http.StatusMethodNotAllowed)
		return
	}
	if h.FanqieImporter == nil {
		http.Error(w, "番茄导入未接入", http.StatusNotImplemented)
		return
	}

	bookID := strings.TrimSpace(r.FormValue("bookId"))
	if bookID == "" {
		http.Error(w, "缺少 bookId", http.StatusBadRequest)
		return
	}

	book := fanqie.Book{
		BookID: bookID,
		Title:  strings.TrimSpace(r.FormValue("title")),
		Author: strings.TrimSpace(r.FormValue("author")),
	}

	if book.Title == "" {
		book.Title = bookID
	}

	log.Printf("番茄导入开始：《%s》（bookId=%s）", book.Title, bookID)

	go func() {
		// 补全元数据（简介、封面地址）
		cli := fanqie.NewClient()
		if info, err := cli.BookInfo(bookID); err == nil {
			book = *info
		}

		if _, err := h.FanqieImporter(book); err != nil {
			log.Printf("番茄导入《%s》失败: %v", book.Title, err)
			return
		}
		log.Printf("番茄导入《%s》完成", book.Title)
	}()

	http.Redirect(w, r, "/", http.StatusSeeOther)
}

// fanqieUpdate 手动触发一本番茄书的追更检查。
func (h *Handler) fanqieUpdate(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "只支持 POST", http.StatusMethodNotAllowed)
		return
	}
	if h.FanqieUpdater == nil {
		http.Error(w, "追更未接入", http.StatusNotImplemented)
		return
	}

	id := int64(atoiDef(r.FormValue("id"), 0))
	if id <= 0 {
		http.Error(w, "缺少 id", http.StatusBadRequest)
		return
	}

	book, err := h.Store.GetBook(id)
	if err != nil {
		http.Error(w, "书不存在", http.StatusNotFound)
		return
	}

	go func(b *db.Book) {
		updated, added, err := h.FanqieUpdater(b)
		if err != nil {
			log.Printf("追更《%s》失败: %v", b.Title, err)
			return
		}
		if updated {
			log.Printf("追更《%s》：新增 %d 章", b.Title, added)
		} else {
			log.Printf("追更《%s》：已是最新（%d 章）", b.Title, b.Chapters)
		}
	}(book)

	http.Redirect(w, r, "/", http.StatusSeeOther)
}
