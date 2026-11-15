// Package web 提供管理界面（书单、上传、任务）。
package web

import (
	"fmt"
	"html/template"
	"io"
	"log"
	"net/http"
	"strings"

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
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="dark">
<title>ClockReader 书房</title>
<style>
 *{box-sizing:border-box}
 :root{
  --bg:#080c12;--panel-a:#111a24;--panel-b:#0d141d;--line:#1d2937;
  --text:#e9eff7;--muted:#8496a9;--dim:#63788c;
  --accent:#1585c0;--accent-lit:#1ba3e6;--accent-soft:rgba(21,133,192,.16);
  --danger:#e5484d;
 }
 html,body{min-height:100%}
 body{
  margin:0;color:var(--text);font-size:14px;line-height:1.55;
  font-family:-apple-system,"Segoe UI","PingFang SC","Microsoft YaHei",system-ui,sans-serif;
  background:
   radial-gradient(900px 480px at 10% -12%,rgba(21,133,192,.20),transparent 62%),
   radial-gradient(720px 420px at 102% -6%,rgba(92,72,220,.12),transparent 60%),
   var(--bg);
  background-attachment:fixed;-webkit-font-smoothing:antialiased;
 }
 .wrap{max-width:1060px;margin:0 auto;padding:30px 20px 60px}
 .muted{color:var(--muted)}
 .nowrap{white-space:nowrap}

 /* 顶部品牌区 */
 .head{display:flex;align-items:center;justify-content:space-between;gap:16px;flex-wrap:wrap}
 .brand{display:flex;align-items:center;gap:13px}
 .logo{
  width:44px;height:44px;flex:0 0 auto;border-radius:12px;
  display:flex;align-items:center;justify-content:center;
  font-size:19px;font-weight:600;color:#fff;letter-spacing:.5px;
  background:linear-gradient(145deg,#1ba3e6,#1585c0 55%,#0f6a9c);
  box-shadow:0 6px 18px rgba(21,133,192,.35),inset 0 1px 0 rgba(255,255,255,.25);
 }
 h1{font-size:19px;font-weight:600;margin:0;letter-spacing:.2px}
 .brand p{margin:2px 0 0;font-size:12.5px}
 .chip{
  display:inline-flex;align-items:center;gap:8px;
  padding:7px 13px;border-radius:999px;font-size:12.5px;
  color:#a9bccd;background:rgba(255,255,255,.03);
  border:1px solid var(--line);font-family:ui-monospace,SFMono-Regular,Consolas,monospace;
 }
 .chip .dot{width:7px;height:7px;border-radius:50%;background:#2ecc71;box-shadow:0 0 0 3px rgba(46,204,113,.15)}

 /* 概览数字 */
 .stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:12px;margin:24px 0 18px}
 .stat{
  padding:14px 16px;border-radius:13px;border:1px solid var(--line);
  background:linear-gradient(180deg,var(--panel-a),var(--panel-b));
  box-shadow:0 8px 24px rgba(0,0,0,.25);
 }
 .stat-k{display:block;font-size:12px;color:var(--dim);letter-spacing:.04em}
 .stat-v{display:block;margin-top:4px;font-size:22px;font-weight:600;font-variant-numeric:tabular-nums}

 /* 卡片 */
 .card{
  border:1px solid var(--line);border-radius:14px;padding:18px 20px;
  background:linear-gradient(180deg,var(--panel-a),var(--panel-b));
  box-shadow:0 10px 30px rgba(0,0,0,.28);margin-bottom:16px;
 }
 .card-head{margin-bottom:14px}
 .card-head.row{display:flex;align-items:baseline;justify-content:space-between;gap:12px}
 h2{font-size:15px;font-weight:600;margin:0}
 .card-head p{margin:4px 0 0;font-size:12.5px}

 /* 上传区 */
 .drop{
  display:flex;align-items:center;gap:14px;flex-wrap:wrap;
  padding:16px;border:1px dashed #263546;border-radius:12px;
  background:rgba(255,255,255,.015);
  transition:border-color .18s,background .18s,box-shadow .18s;
 }
 .drop:hover{border-color:rgba(21,133,192,.55);background:rgba(21,133,192,.05)}
 .drop.over{
  border-color:rgba(21,133,192,.85);background:rgba(21,133,192,.11);
  box-shadow:inset 0 0 0 1px rgba(21,133,192,.35),0 0 0 4px rgba(21,133,192,.08);
 }
 .drop-note{flex:1 1 100%;margin:2px 0 0;font-size:12.5px;color:var(--dim)}
 .drop-note.bad{color:#ff9ea2}
 input[type=file]{flex:1 1 240px;min-width:0;color:var(--muted);font-size:13px}
 input[type=file]::file-selector-button{
  margin-right:12px;padding:8px 14px;border-radius:9px;cursor:pointer;
  font-family:inherit;font-size:13px;color:#cfe8f7;
  background:var(--accent-soft);border:1px solid rgba(21,133,192,.42);
  transition:background .18s,color .18s;
 }
 input[type=file]::file-selector-button:hover{background:rgba(21,133,192,.30);color:#fff}
 button{
  padding:9px 20px;border:0;border-radius:9px;cursor:pointer;
  font-family:inherit;font-size:13.5px;font-weight:500;color:#fff;
  background:linear-gradient(180deg,#1a90ce,#1585c0);
  box-shadow:0 5px 16px rgba(21,133,192,.30),inset 0 1px 0 rgba(255,255,255,.18);
  transition:filter .18s,transform .18s;
 }
 button:hover{filter:brightness(1.12);transform:translateY(-1px)}
 button:active{transform:translateY(0);filter:brightness(.96)}

 /* 表格 */
 .table-wrap{overflow-x:auto;margin:0 -4px;padding:0 4px}
 table{width:100%;border-collapse:collapse}
 th{
  position:sticky;top:0;z-index:1;text-align:left;font-weight:500;font-size:12px;
  letter-spacing:.03em;color:var(--dim);padding:8px 12px;
  background:var(--panel-b);border-bottom:1px solid var(--line);
 }
 td{padding:11px 12px;border-bottom:1px solid rgba(255,255,255,.055);vertical-align:middle}
 tbody tr:last-child td{border-bottom:0}
 tbody tr{transition:background .14s}
 tbody tr:hover{background:rgba(21,133,192,.07)}
 td.title{font-weight:500;max-width:300px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
 .num{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}
 th.num{text-align:right}
 .tag{
  display:inline-block;padding:2px 9px;border-radius:999px;font-size:11.5px;
  color:#7cc4ea;background:var(--accent-soft);border:1px solid rgba(21,133,192,.34);
 }
 .tag-fanqie{color:#f0b072;background:rgba(240,162,74,.14);border-color:rgba(240,162,74,.34)}

 /* 空状态 */
 .empty{padding:52px 0 46px;text-align:center}
 .empty svg{opacity:.5;margin-bottom:12px}
 .empty p{margin:0;font-size:14px}
 .empty p.muted{margin-top:5px;font-size:12.5px}

 /* 错误提示 */
 .err{
  display:flex;align-items:center;gap:9px;margin:0 0 16px;padding:11px 14px;
  border-radius:10px;font-size:13px;color:#ff9ea2;
  background:rgba(229,72,77,.10);border:1px solid rgba(229,72,77,.34);
 }
 .err::before{content:"";width:6px;height:6px;border-radius:50%;background:var(--danger);flex:0 0 auto}

 .foot{margin-top:22px;text-align:center;font-size:12px;color:var(--dim)}

 @media (max-width:720px){
  .wrap{padding:22px 14px 46px}
  .col-source,.col-time{display:none}
  .chip{font-size:11.5px;padding:6px 11px}
 }
 @media (max-width:560px){
  .col-author,.col-chapters{display:none}
  td.title{max-width:200px}
 }
</style></head><body>
<div class="wrap">

<header class="head">
  <div class="brand">
    <div class="logo">书</div>
    <div>
      <h1>ClockReader 书房</h1>
      <p class="muted">局域网书库 · 上传 EPUB 后由设备自动拉取</p>
    </div>
  </div>
  <div class="chip"><span class="dot"></span>{{.Hint}}</div>
</header>

{{if .Err}}<p class="err">{{.Err}}</p>{{end}}

<section class="stats">
  <div class="stat"><span class="stat-k">在库书籍</span><span class="stat-v">{{.Count}}</span></div>
  <div class="stat"><span class="stat-k">总章节</span><span class="stat-v">{{.TotalChapters}}</span></div>
  <div class="stat"><span class="stat-k">占用空间</span><span class="stat-v">{{.TotalBytes}}</span></div>
</section>

<section class="card">
  <div class="card-head">
    <h2>上传 EPUB</h2>
    <p class="muted">文件名建议写成「书名 - 作者.epub」，同名书会被覆盖更新</p>
  </div>
  <form method="post" action="/upload" enctype="multipart/form-data" class="drop" id="drop-form">
    <input type="file" name="file" accept=".epub" required id="drop-file">
    <button type="submit">上传并转换</button>
    <p class="drop-note" id="drop-note">把 EPUB 文件拖到这里，松手即上传</p>
  </form>
</section>

<section class="card">
  <div class="card-head row">
    <h2>书库</h2>
    <span class="muted">{{.Count}} 本</span>
  </div>
  {{if .Books}}
  <div class="table-wrap">
  <table>
  <thead><tr><th>书名</th><th class="col-author">作者</th><th class="num col-chapters">章节</th><th class="num">大小</th><th class="col-source">来源</th><th class="col-time">更新时间</th></tr></thead>
  <tbody>
  {{range .Books}}
  <tr>
    <td class="title">{{.Title}}</td>
    <td class="muted col-author">{{if .Author}}{{.Author}}{{else}}—{{end}}</td>
    <td class="num col-chapters">{{.Chapters}}</td>
    <td class="num">{{.BytesHuman}}</td>
    <td class="col-source"><span class="tag tag-{{.Source}}">{{.SourceLabel}}</span></td>
    <td class="muted nowrap col-time">{{.UpdatedAt}}</td>
  </tr>
  {{end}}
  </tbody></table>
  </div>
  {{else}}
  <div class="empty">
    <svg width="46" height="46" viewBox="0 0 24 24" fill="none" stroke="#1585c0" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round">
      <path d="M4 4.5A2.5 2.5 0 0 1 6.5 2H20v18H6.5A2.5 2.5 0 0 0 4 22z"/>
      <path d="M4 17.5A2.5 2.5 0 0 1 6.5 15H20"/>
    </svg>
    <p>书库还是空的</p>
    <p class="muted">上传第一本 EPUB，设备连上后会自动同步</p>
  </div>
  {{end}}
</section>

<p class="foot">ClockReader Server · 设备通过 /api/ 接口按 EBF 文件粒度拉取书库</p>

</div>
<script>
// 拖拽上传：拖进虚线区域松手即提交；页面其它位置一律拦截，避免浏览器直接打开文件
(function(){
  var form = document.getElementById('drop-form');
  var input = document.getElementById('drop-file');
  var note = document.getElementById('drop-note');
  var hint = note.textContent;

  function setNote(text, bad){
    note.textContent = text;
    note.className = bad ? 'drop-note bad' : 'drop-note';
  }

  ['dragenter','dragover'].forEach(function(t){
    form.addEventListener(t, function(e){
      e.preventDefault();
      e.stopPropagation();
      form.classList.add('over');
    });
  });
  ['dragleave','dragend'].forEach(function(t){
    form.addEventListener(t, function(e){
      if (e.target !== form) return;
      form.classList.remove('over');
    });
  });

  form.addEventListener('drop', function(e){
    e.preventDefault();
    e.stopPropagation();
    form.classList.remove('over');

    var files = e.dataTransfer && e.dataTransfer.files;
    if (!files || !files.length) return;
    var file = files[0];
    if (!/\.epub$/i.test(file.name)) {
      setNote('只支持 .epub 文件：' + file.name, true);
      return;
    }

    // 把拖进来的文件塞进 input，沿用原来的表单提交流程
    try {
      var dt = new DataTransfer();
      dt.items.add(file);
      input.files = dt.files;
    } catch (err) {
      input.files = files;
    }
    setNote('已选择：' + file.name + '，正在上传…');
    if (form.requestSubmit) form.requestSubmit(); else form.submit();
  });

  input.addEventListener('change', function(){
    if (input.files && input.files[0]) setNote('已选择：' + input.files[0].name + '，点「上传并转换」开始');
    else setNote(hint);
  });

  // 拖到页面其它位置时保持静默，不让浏览器把文件当网址打开
  window.addEventListener('dragover', function(e){ e.preventDefault(); });
  window.addEventListener('drop', function(e){ e.preventDefault(); });
})();
</script>
</body></html>`))

// BookRow 是模板用的行视图。
type BookRow struct {
	Title, Author, Source, SourceLabel, UpdatedAt string
	Chapters                                      int
	BytesHuman                                    string
}

// sourceLabel 把数据库里的来源标识换成中文，未知值原样展示。
func sourceLabel(s string) string {
	switch s {
	case "upload":
		return "上传"
	case "fanqie":
		return "番茄"
	case "":
		return "未知"
	default:
		return s
	}
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

// listenHint 把监听地址拼成给人看的管理页地址。
// 配置里通常写 ":8080"，此时主机名留空，由用户自己换成服务器 IP。
func listenHint(addr string) string {
	if strings.HasPrefix(addr, ":") {
		return "http://<本机 IP>" + addr
	}
	if addr == "" {
		return "http://<本机 IP>"
	}
	return "http://" + addr
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
		var totalBytes int64
		totalChapters := 0
		for _, b := range books {
			totalBytes += b.Bytes
			totalChapters += b.Chapters
			rows = append(rows, BookRow{
				Title: b.Title, Author: b.Author, Source: b.Source,
				SourceLabel: sourceLabel(b.Source),
				UpdatedAt:   b.UpdatedAt, Chapters: b.Chapters,
				BytesHuman: humanBytes(b.Bytes),
			})
		}
		data := struct {
			Hint          string
			Books         []BookRow
			Err           string
			Count         int
			TotalChapters int
			TotalBytes    string
		}{
			Hint: listenHint(addr), Books: rows, Err: errMsg,
			Count: len(rows), TotalChapters: totalChapters,
			TotalBytes: humanBytes(totalBytes),
		}
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
