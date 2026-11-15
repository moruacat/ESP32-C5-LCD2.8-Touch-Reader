// Package web 提供管理界面（书库、导入任务、任务日志）。
package web

import (
	"encoding/json"
	"fmt"
	"html/template"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
	"time"

	"clockreader-server/api"
	"clockreader-server/db"
	"clockreader-server/fanqie"
)

// Handler 持有数据层与上传依赖。
type Handler struct {
	Store *db.Store
	// Importer 在 EPUB 上传后被调用（转换 + 入库）。
	Importer func(name string, data []byte) (*db.Book, error)
	// FanqieImporter 后台抓一本番茄书并入库。
	FanqieImporter func(book fanqie.Book) (*db.Book, error)
	// FanqieUpdater 检查一本番茄书有没有新章。
	FanqieUpdater func(book *db.Book) (bool, int, error)
	// Logs 是服务端日志缓冲，管理页底部轮询显示。
	Logs *LogBuffer
	// Addr 是监听地址，用于在页面顶部显示访问入口。
	Addr string
}

const perPage = 24

// tmplFuncs 是模板里用到的小工具（分页换页要用加减）。
var tmplFuncs = template.FuncMap{
	"add": func(a, b int) int { return a + b },
	"sub": func(a, b int) int { return a - b },
}

var indexTmpl = template.Must(template.New("index").Funcs(tmplFuncs).Parse(`<!doctype html>
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
  --ok:#2ecc71;--danger:#e5484d;--warn:#f0a020;
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
 .wrap{max-width:1180px;margin:0 auto;padding:26px 20px 48px}
 .muted{color:var(--muted)}
 .nowrap{white-space:nowrap}

 /* 顶部 */
 .head{display:flex;align-items:center;justify-content:space-between;gap:16px;flex-wrap:wrap}
 .brand{display:flex;align-items:center;gap:13px}
 .logo{
  width:44px;height:44px;flex:0 0 auto;border-radius:12px;
  display:flex;align-items:center;justify-content:center;font-size:20px;color:#fff;
  background:linear-gradient(145deg,#1ba3e6,#1585c0 55%,#0f6a9c);
  box-shadow:0 6px 18px rgba(21,133,192,.35),inset 0 1px 0 rgba(255,255,255,.25);
 }
 h1{font-size:19px;font-weight:600;margin:0;letter-spacing:.2px}
 .brand p{margin:2px 0 0;font-size:12.5px}
 .chip{
  display:inline-flex;align-items:center;gap:8px;padding:7px 13px;border-radius:999px;
  font-size:12.5px;color:#a9bccd;background:rgba(255,255,255,.03);
  border:1px solid var(--line);font-family:ui-monospace,Consolas,monospace;
 }
 .chip .dot{width:7px;height:7px;border-radius:50%;background:var(--ok);
  box-shadow:0 0 0 3px rgba(46,204,113,.15)}

 /* 统计 */
 .stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px;margin:22px 0 16px}
 .stat{
  padding:14px 16px;border-radius:13px;border:1px solid var(--line);
  background:linear-gradient(180deg,var(--panel-a),var(--panel-b));
  box-shadow:0 8px 24px rgba(0,0,0,.25);position:relative;overflow:hidden;
 }
 .stat::after{content:"";position:absolute;right:-26px;top:-30px;width:86px;height:86px;
  border-radius:50%;background:var(--accent-soft)}
 .stat-k{display:block;font-size:12px;color:var(--dim);letter-spacing:.04em}
 .stat-v{display:block;margin-top:4px;font-size:22px;font-weight:600;font-variant-numeric:tabular-nums}

 /* 卡片 */
 .card{
  border:1px solid var(--line);border-radius:14px;padding:16px 18px;
  background:linear-gradient(180deg,var(--panel-a),var(--panel-b));
  box-shadow:0 10px 30px rgba(0,0,0,.28);margin-bottom:16px;
 }
 .card-head{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:12px;flex-wrap:wrap}
 h2{font-size:15px;font-weight:600;margin:0}
 .card-head p{margin:2px 0 0;font-size:12.5px}

 /* 两栏 */
 .cols{display:grid;grid-template-columns:minmax(0,2.1fr) minmax(280px,1fr);gap:16px;align-items:start}
 @media (max-width:900px){ .cols{grid-template-columns:1fr} }

 /* 工具条 */
 .tools{display:flex;gap:8px;flex-wrap:wrap}
 .tools input,.tools select{
  height:32px;border-radius:8px;border:1px solid var(--line);
  background:#0b121a;color:var(--text);padding:0 10px;font-size:13px;outline:none;
 }
 .tools input:focus,.tools select:focus{border-color:var(--accent)}
 .tools input{width:190px}
 .btn{
  display:inline-flex;align-items:center;justify-content:center;gap:6px;
  height:32px;padding:0 13px;border:0;border-radius:8px;cursor:pointer;
  font-size:13px;color:#fff;background:var(--accent);transition:.16s;
 }
 .btn:hover{background:var(--accent-lit)}
 .btn-ghost{background:rgba(255,255,255,.05);color:#cddbe8;border:1px solid var(--line)}
 .btn-ghost:hover{background:rgba(255,255,255,.09)}
 .btn-del{height:26px;padding:0 10px;font-size:12px;border-radius:6px;
  background:rgba(229,72,77,.14);color:#f08589;border:1px solid rgba(229,72,77,.25)}
 .btn-del:hover{background:rgba(229,72,77,.24)}
 .btn-sync{height:26px;padding:0 10px;font-size:12px;border-radius:6px;
  background:rgba(21,133,192,.16);color:#6fc6f0;border:1px solid rgba(21,133,192,.3)}
 .btn-sync:hover{background:rgba(21,133,192,.28)}

 /* 表格 */
 .table-wrap{margin:0 -6px;overflow-x:auto}
 table{border-collapse:collapse;width:100%}
 th,td{text-align:left;padding:9px 10px;font-size:13.5px;vertical-align:middle}
 th{color:var(--dim);font-weight:500;font-size:12px;letter-spacing:.03em;
  border-bottom:1px solid var(--line);white-space:nowrap}
 tbody tr{border-bottom:1px solid rgba(29,41,55,.55)}
 tbody tr:hover{background:rgba(21,133,192,.06)}
 td.title{font-weight:500}
 .cover{
  width:34px;height:46px;border-radius:5px;object-fit:cover;flex:0 0 auto;
  border:1px solid var(--line);background:#0b121a;display:block;
 }
 .cover-ph{width:34px;height:46px;border-radius:5px;flex:0 0 auto;display:flex;
  align-items:center;justify-content:center;color:var(--dim);font-size:11px;
  background:#0b121a;border:1px solid var(--line)}
 .title-cell{display:flex;align-items:center;gap:10px;min-width:0}
 .title-cell span{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
 .num{text-align:right;font-variant-numeric:tabular-nums;color:#c6d5e3}
 .tag{display:inline-block;padding:2px 8px;border-radius:6px;font-size:11.5px;
  font-weight:500;white-space:nowrap}
 .tag-upload{background:rgba(21,133,192,.18);color:#6fc6f0}
 .tag-fanqie{background:rgba(229,72,77,.16);color:#f08589}
 .tag-other{background:rgba(255,255,255,.07);color:#a9bccd}

 /* 分页 */
 .pager{display:flex;align-items:center;justify-content:space-between;gap:10px;
  margin-top:14px;flex-wrap:wrap}
 .pages{display:flex;gap:6px;align-items:center}
 .page{min-width:30px;height:30px;display:inline-flex;align-items:center;justify-content:center;
  padding:0 8px;border-radius:8px;font-size:13px;color:#b9c9d8;text-decoration:none;
  border:1px solid transparent}
 .page:hover{background:rgba(255,255,255,.06)}
 .page.on{background:var(--accent);color:#fff}
 .page.dis{color:var(--dim);pointer-events:none}

 /* 空态 */
 .empty{padding:36px 0;text-align:center;color:var(--muted)}
 .empty p{margin:8px 0}
 .err{color:var(--danger);margin:10px 0;font-size:13px}

 /* 导入任务 */
 .drop{
  border:1.5px dashed #2a3a4d;border-radius:12px;padding:22px 16px;text-align:center;
  transition:.18s;cursor:pointer;background:rgba(11,18,26,.5)
 }
 .drop.over{border-color:var(--accent);background:rgba(21,133,192,.09)}
 .drop svg{display:block;margin:0 auto 8px}
 .drop p{margin:0;font-size:13.5px}
 .drop small{display:block;margin-top:4px;color:var(--dim);font-size:12px}
 .drop input{display:none}
 .pick{font-size:12.5px;color:var(--dim);margin:8px 0 0;text-align:center;word-break:break-all}
 .jobs{list-style:none;margin:14px 0 0;padding:0}
 .jobs li{display:flex;align-items:flex-start;gap:9px;padding:8px 0;
  border-bottom:1px solid rgba(29,41,55,.5);font-size:13px}
 .jobs li:last-child{border-bottom:0}
 .ji{flex:0 0 auto;width:18px;height:18px;border-radius:50%;display:flex;
  align-items:center;justify-content:center;font-size:11px;margin-top:1px}
 .ji-ok{background:rgba(46,204,113,.16);color:var(--ok)}
 .ji-bad{background:rgba(229,72,77,.16);color:var(--danger)}
 .ji-run{background:rgba(21,133,192,.16);color:var(--accent-lit)}
 .jobs .t{flex:1;min-width:0}
 .jobs .t b{display:block;font-weight:500;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
 .jobs .t span{color:var(--dim);font-size:12px}
 .sync-row{display:flex;align-items:flex-start;gap:9px;padding:8px 0;font-size:13px}
 .sync-row .t b{display:block;font-weight:500}
 .sync-row .t span{color:var(--dim);font-size:12px}

 /* 日志 */
 .log-head{display:flex;align-items:center;justify-content:space-between;margin-bottom:10px}
 .log{
  height:190px;overflow-y:auto;background:#070b10;border:1px solid var(--line);
  border-radius:10px;padding:10px 12px;
  font-family:ui-monospace,SFMono-Regular,Consolas,monospace;font-size:12.5px;line-height:1.7;
 }
 .log div{white-space:pre-wrap;word-break:break-word;color:#9fb2c4}
 .log .ts{color:#46617a;margin-right:8px}
 .log .ok{color:#5fd08a}
 .log .bad{color:#ef7b7f}
 .log::-webkit-scrollbar{width:8px}
 .log::-webkit-scrollbar-thumb{background:#1c2836;border-radius:4px}

 .foot{margin-top:18px;text-align:center;color:var(--dim);font-size:12px}
 @media (max-width:640px){ .col-author,.col-size{display:none} .tools input{width:130px} }
</style></head><body>
<div class="wrap">

<header class="head">
  <div class="brand">
    <div class="logo">书</div>
    <div>
      <h1>ClockReader 书房</h1>
      <p class="muted">局域网书库管理 · 设备同步中心</p>
    </div>
  </div>
  <div class="chip"><span class="dot"></span>{{.Hint}}</div>
</header>

{{if .Err}}<p class="err">{{.Err}}</p>{{end}}

<section class="stats">
  <div class="stat"><span class="stat-k">在库书籍</span><span class="stat-v">{{.Count}} 本</span></div>
  <div class="stat"><span class="stat-k">总章节</span><span class="stat-v">{{.TotalChapters}} 章</span></div>
  <div class="stat"><span class="stat-k">占用空间</span><span class="stat-v">{{.TotalBytes}}</span></div>
</section>

<div class="cols">

  <!-- 左：书库 -->
  <div>
  <section class="card">
    <div class="card-head">
      <h2>书库</h2>
      <form class="tools" method="get" action="/">
        <input type="search" name="q" value="{{.Search}}" placeholder="搜索书名、作者...">
        <select name="src" onchange="this.form.submit()">
          <option value="" {{if eq .Source ""}}selected{{end}}>全部来源</option>
          <option value="upload" {{if eq .Source "upload"}}selected{{end}}>上传</option>
          <option value="fanqie" {{if eq .Source "fanqie"}}selected{{end}}>番茄</option>
        </select>
        <select name="sort" onchange="this.form.submit()">
          <option value="recent" {{if eq .Sort "recent"}}selected{{end}}>最近更新</option>
          <option value="title" {{if eq .Sort "title"}}selected{{end}}>按书名</option>
        </select>
        <button class="btn btn-ghost" type="submit">搜索</button>
      </form>
    </div>

    {{if .Books}}
    <div class="table-wrap">
    <table>
    <thead><tr>
      <th>书名</th>
      <th class="col-author">作者</th>
      <th class="num">章节数</th>
      <th class="num col-size">大小</th>
      <th>来源</th>
      <th class="nowrap">更新时间</th>
      <th>操作</th>
    </tr></thead>
    <tbody>
    {{range .Books}}
    <tr>
      <td class="title">
        <div class="title-cell">
          {{if .HasCover}}<img class="cover" src="/cover?id={{.ID}}" alt="" loading="lazy">
          {{else}}<div class="cover-ph">无</div>{{end}}
          <span title="{{.Title}}">{{.Title}}</span>
        </div>
      </td>
      <td class="muted col-author">{{if .Author}}{{.Author}}{{else}}—{{end}}</td>
      <td class="num">{{.Chapters}}</td>
      <td class="num col-size">{{.BytesHuman}}</td>
      <td><span class="tag tag-{{.Source}}">{{.SourceLabel}}</span></td>
      <td class="muted nowrap">{{.UpdatedAt}}</td>
      <td class="nowrap">
        {{if eq .Source "fanqie"}}
        <form method="post" action="/fanqie/update" style="margin:0;display:inline"
              onsubmit="return confirm('检查《{{.Title}}》有没有新章？')">
          <input type="hidden" name="id" value="{{.ID}}">
          <button class="btn btn-sync" type="submit">更新</button>
        </form>
        {{end}}
        <form method="post" action="/delete" style="margin:0;display:inline"
              onsubmit="return confirm('确定删除《{{.Title}}》？文件会一并删除，设备端已下载的副本不受影响。')">
          <input type="hidden" name="id" value="{{.ID}}">
          <button class="btn btn-del" type="submit">删除</button>
        </form>
      </td>
    </tr>
    {{end}}
    </tbody></table>
    </div>

    <div class="pager">
      <span class="muted">共 {{.Total}} 本书{{if .Filtered}}（已筛选）{{end}}</span>
      {{if gt .Pages 1}}
      <div class="pages">
        {{if gt .Page 1}}<a class="page" href="{{.PageURL}}page={{sub .Page 1}}">‹</a>
        {{else}}<span class="page dis">‹</span>{{end}}
        {{range .PageLinks}}
          {{if .Current}}<span class="page on">{{.No}}</span>
          {{else if .Gap}}<span class="page dis">…</span>
          {{else}}<a class="page" href="{{$.PageURL}}page={{.No}}">{{.No}}</a>{{end}}
        {{end}}
        {{if lt .Page .Pages}}<a class="page" href="{{.PageURL}}page={{add .Page 1}}">›</a>
        {{else}}<span class="page dis">›</span>{{end}}
      </div>
      {{end}}
    </div>
    {{else}}
    <div class="empty">
      <p>书库还是空的</p>
      <p class="muted">在右侧上传第一本 EPUB，设备连上后会自动同步</p>
    </div>
    {{end}}
  </section>
  </div>

  <!-- 右：导入任务 -->
  <div>
  <section class="card">
    <div class="card-head"><h2>导入任务</h2></div>

    <form method="post" action="/upload" enctype="multipart/form-data" id="drop-form">
      <label class="drop" id="drop-zone">
        <input type="file" name="file" accept=".epub" required id="drop-file">
        <svg width="34" height="34" viewBox="0 0 24 24" fill="none" stroke="#4aa3d8"
             stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round">
          <path d="M12 16V4m0 0L7.5 8.5M12 4l4.5 4.5"/>
          <path d="M4 16v2.5A1.5 1.5 0 0 0 5.5 20h13a1.5 1.5 0 0 0 1.5-1.5V16"/>
        </svg>
        <p>拖放 EPUB 文件到此处</p>
        <small>或点击选择文件</small>
      </label>
      <p class="pick" id="drop-note">同名书会被覆盖更新</p>
      <button class="btn" type="submit" style="width:100%;margin-top:10px;height:38px">
        ▶ 开始导入
      </button>
      <a class="btn btn-ghost" href="/fanqie"
         style="width:100%;margin-top:8px;height:34px;justify-content:center;text-decoration:none">
        🔍 番茄小说搜索
      </a>
    </form>

    <div class="card-head" style="margin:16px 0 2px">
      <h2 style="font-size:13px;color:var(--muted)">设备同步</h2>
    </div>
    <div class="sync-row">
      <span class="ji {{if .DeviceOnline}}ji-ok{{else}}ji-run{{end}}">⟳</span>
      <span class="t"><b>{{.DeviceText}}</b><span>{{.Hint}}</span></span>
    </div>

    <div class="card-head" style="margin:16px 0 2px">
      <h2 style="font-size:13px;color:var(--muted)">最近任务</h2>
    </div>
    <ul class="jobs">
      {{range .Jobs}}
      <li>
        <span class="ji {{if eq .Status "done"}}ji-ok{{else if eq .Status "fail"}}ji-bad{{else}}ji-run{{end}}">
          {{if eq .Status "done"}}✓{{else if eq .Status "fail"}}✕{{else}}⟳{{end}}
        </span>
        <span class="t"><b>{{.Title}}</b><span>{{.Detail}}</span></span>
      </li>
      {{else}}
      <li><span class="ji ji-run">·</span>
        <span class="t"><b>还没有任务</b><span>上传一本 EPUB 试试</span></span></li>
      {{end}}
    </ul>
  </section>
  </div>

</div>

<!-- 底部：任务日志 -->
<section class="card">
  <div class="log-head">
    <h2>任务日志</h2>
    <button class="btn btn-ghost" id="log-clear" type="button">清空</button>
  </div>
  <div class="log" id="log-box"></div>
</section>

<p class="foot">ClockReader Server · 设备通过 /api/ 接口按 EBF 文件粒度拉取书库</p>

</div>
<script>
// ---- 拖拽上传：拖进虚线区松手即提交 ----
(function(){
  var form = document.getElementById('drop-form');
  var zone = document.getElementById('drop-zone');
  var input = document.getElementById('drop-file');
  var note = document.getElementById('drop-note');
  var hint = note.textContent;

  function setNote(text, bad){
    note.textContent = text;
    note.style.color = bad ? 'var(--danger)' : 'var(--dim)';
  }

  ['dragenter','dragover'].forEach(function(t){
    zone.addEventListener(t, function(e){
      e.preventDefault(); e.stopPropagation();
      zone.classList.add('over');
    });
  });
  ['dragleave','dragend'].forEach(function(t){
    zone.addEventListener(t, function(e){
      e.preventDefault(); zone.classList.remove('over');
    });
  });
  zone.addEventListener('drop', function(e){
    e.preventDefault(); e.stopPropagation();
    zone.classList.remove('over');
    var files = e.dataTransfer && e.dataTransfer.files;
    if (!files || !files.length) return;
    var file = files[0];
    if (!/\.epub$/i.test(file.name)) {
      setNote('只支持 .epub 文件：' + file.name, true);
      return;
    }
    try {
      var dt = new DataTransfer();
      dt.items.add(file);
      input.files = dt.files;
    } catch (err) { input.files = files; }
    setNote('上传中：' + file.name + ' …');
    if (form.requestSubmit) form.requestSubmit(); else form.submit();
  });
  input.addEventListener('change', function(){
    if (input.files && input.files[0]) setNote('已选择：' + input.files[0].name);
    else setNote(hint);
  });
  window.addEventListener('dragover', function(e){ e.preventDefault(); });
  window.addEventListener('drop', function(e){ e.preventDefault(); });
})();

// ---- 任务日志：每 3 秒轮询一次 ----
(function(){
  var box = document.getElementById('log-box');
  var clearBtn = document.getElementById('log-clear');

  function esc(s){
    return s.replace(/[&<>]/g, function(c){
      return {'&':'&amp;','<':'&lt;','>':'&gt;'}[c];
    });
  }

  function render(list){
    var html = '';
    for (var i = 0; i < list.length; i++) {
      var l = list[i];
      var cls = /失败|错误|error|fail/i.test(l.text) ? 'bad'
              : /完成|成功|导入|发现/i.test(l.text) ? 'ok' : '';
      html += '<div><span class="ts">[' + l.at + ']</span><span class="' + cls + '">'
            + esc(l.text) + '</span></div>';
    }
    box.innerHTML = html || '<div class="muted">（暂无日志）</div>';
    box.scrollTop = box.scrollHeight;
  }

  function poll(){
    fetch('/api/logs', {cache:'no-store'})
      .then(function(r){ return r.ok ? r.json() : Promise.reject(); })
      .then(function(d){ render(d.logs || []); })
      .catch(function(){});
  }

  clearBtn.addEventListener('click', function(){
    fetch('/api/logs/clear', {method:'POST'}).then(poll).catch(function(){});
  });

  poll();
  setInterval(poll, 3000);
})();
</script>
</body></html>`))

// BookRow 是模板用的行视图。
type BookRow struct {
	ID          int64
	Title       string
	Author      string
	Source      string
	SourceLabel string
	UpdatedAt   string
	Chapters    int
	BytesHuman  string
	HasCover    bool
}

// jobRow 是「最近任务」的样子。
type jobRow struct {
	Status string // done / fail / run
	Title  string
	Detail string
}

// pageLink 用于分页条。
type pageLink struct {
	No      int
	Current bool
	Gap     bool
}

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

func sourceClass(s string) string {
	switch s {
	case "upload":
		return "upload"
	case "fanqie":
		return "fanqie"
	default:
		return "other"
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

func listenHint(addr string) string {
	if strings.HasPrefix(addr, ":") {
		return "http://<本机 IP>" + addr
	}
	if addr == "" {
		return "http://<本机 IP>"
	}
	return "http://" + addr
}

func atoiDef(s string, def int) int {
	n, err := strconv.Atoi(s)
	if err != nil {
		return def
	}
	return n
}

// deviceStatus 把「设备最后一次访问」变成一句话。
// 2 分钟内访问过算在线（设备刷新书单/下载章节都会刷新时间）。
func deviceStatus() (online bool, text string) {
	seen := api.LastSeen()
	if seen == 0 {
		return false, "尚未收到设备请求"
	}
	d := time.Since(time.Unix(seen, 0))
	switch {
	case d < 2*time.Minute:
		return true, "设备在线 · 刚刚同步"
	case d < time.Hour:
		return true, fmt.Sprintf("设备在线 · %d 分钟前同步", int(d.Minutes()))
	case d < 24*time.Hour:
		return false, fmt.Sprintf("%d 小时前同步", int(d.Hours()))
	default:
		return false, fmt.Sprintf("%d 天前同步", int(d.Hours()/24))
	}
}

// buildPages 生成分页条：首页、末页、当前页左右各 1 页，其余用省略号。
func buildPages(cur, total int) []pageLink {
	var out []pageLink
	if total <= 1 {
		return out
	}
	emit := func(n int) { out = append(out, pageLink{No: n, Current: n == cur}) }
	gap := func() { out = append(out, pageLink{Gap: true}) }

	emit(1)
	if cur > 3 {
		gap()
	}
	for n := cur - 1; n <= cur+1; n++ {
		if n > 1 && n < total {
			emit(n)
		}
	}
	if cur < total-2 {
		gap()
	}
	if total > 1 {
		emit(total)
	}
	return out
}

// Register 挂管理页路由。
func (h *Handler) Register(mux *http.ServeMux, addr string) {
	mux.HandleFunc("/", h.index)
	mux.HandleFunc("/upload", h.upload)
	mux.HandleFunc("/delete", h.deleteBook)
	h.registerCover(mux)
	h.registerLogs(mux)
	h.registerFanqie(mux)
}

// deleteBook 删除一本书：先删数据库记录，再删 EBF 目录。
func (h *Handler) deleteBook(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "只支持 POST", http.StatusMethodNotAllowed)
		return
	}

	id := int64(atoiDef(r.FormValue("id"), 0))
	if id <= 0 {
		http.Error(w, "缺少有效的 id", http.StatusBadRequest)
		return
	}

	book, err := h.Store.GetBook(id)
	if err != nil {
		http.Error(w, "书不存在", http.StatusNotFound)
		return
	}

	if err := h.Store.DeleteBook(id); err != nil {
		http.Error(w, "删除失败："+err.Error(), http.StatusInternalServerError)
		return
	}

	// 目录删不掉不算致命：数据库已删，书不会再出现，残留文件下次清理
	if book.EbfDir != "" {
		if err := os.RemoveAll(book.EbfDir); err != nil {
			log.Printf("删除目录失败 %s: %v", book.EbfDir, err)
		}
	}

	log.Printf("已删除 《%s》", book.Title)
	http.Redirect(w, r, "/", http.StatusSeeOther)
}

func (h *Handler) index(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}

	q := r.URL.Query()
	search := strings.TrimSpace(q.Get("q"))
	source := q.Get("src")
	sortBy := q.Get("sort")
	if sortBy == "" {
		sortBy = "recent"
	}
	page := atoiDef(q.Get("page"), 1)
	if page < 1 {
		page = 1
	}

	var errMsg string
	books, total, err := h.Store.ListBooksPage(db.ListOptions{
		Search: search, Source: source, Sort: sortBy,
		Offset: (page - 1) * perPage, Limit: perPage,
	})
	if err != nil {
		errMsg = "读取书库失败: " + err.Error()
	}

	count, chapters, totalBytes, err := h.Store.Stats()
	if err != nil && errMsg == "" {
		errMsg = "统计失败: " + err.Error()
	}

	rows := make([]BookRow, 0, len(books))
	for _, b := range books {
		rows = append(rows, BookRow{
			ID: b.ID, Title: b.Title, Author: b.Author,
			Source: sourceClass(b.Source), SourceLabel: sourceLabel(b.Source),
			UpdatedAt: b.UpdatedAt, Chapters: b.Chapters,
			BytesHuman: humanBytes(b.Bytes),
			HasCover:   b.CoverBytes > 0,
		})
	}

	jobs, _ := h.Store.RecentJobs(5)
	jobRows := make([]jobRow, 0, len(jobs))
	for _, j := range jobs {
		status := "run"
		switch j.Status {
		case "done", "ok", "finished":
			status = "done"
		case "fail", "failed", "error":
			status = "fail"
		}
		title := j.Message
		if title == "" {
			title = "任务 #" + strconv.FormatInt(j.ID, 10)
		}
		detail := j.CreatedAt
		if j.Progress > 0 && j.Progress < 100 {
			detail = fmt.Sprintf("%d%% · %s", j.Progress, detail)
		}
		jobRows = append(jobRows, jobRow{Status: status, Title: title, Detail: detail})
	}

	pages := (total + perPage - 1) / perPage
	if pages < 1 {
		pages = 1
	}
	if page > pages {
		page = pages
	}

	deviceOnline, deviceText := deviceStatus()

	pageURL := "/?"
	if search != "" {
		pageURL += "q=" + url.QueryEscape(search) + "&"
	}
	if source != "" {
		pageURL += "src=" + url.QueryEscape(source) + "&"
	}
	if sortBy != "" {
		pageURL += "sort=" + url.QueryEscape(sortBy) + "&"
	}

	data := struct {
		Hint          string
		Err           string
		Count         int
		TotalChapters int
		TotalBytes    string
		Books         []BookRow
		Total         int
		Page          int
		Pages         int
		PageLinks     []pageLink
		PageURL       string
		Search        string
		Source        string
		Sort          string
		Filtered      bool
		Jobs          []jobRow
		DeviceOnline  bool
		DeviceText    string
	}{
		Hint: listenHint(h.Addr), Err: errMsg,
		Count: count, TotalChapters: chapters, TotalBytes: humanBytes(totalBytes),
		Books: rows, Total: total, Page: page, Pages: pages,
		PageLinks: buildPages(page, pages), PageURL: pageURL,
		Search: search, Source: source, Sort: sortBy,
		Filtered: search != "" || source != "",
		Jobs:     jobRows,
		DeviceOnline: deviceOnline,
		DeviceText:   deviceText,
	}

	if err := indexTmpl.Execute(w, data); err != nil {
		log.Printf("渲染管理页失败: %v", err)
	}
}

// registerLogs 挂日志轮询接口。
func (h *Handler) registerLogs(mux *http.ServeMux) {
	mux.HandleFunc("/api/logs", func(w http.ResponseWriter, r *http.Request) {
		if h.Logs == nil {
			w.Header().Set("Content-Type", "application/json; charset=utf-8")
			_, _ = w.Write([]byte(`{"logs":[]}`))
			return
		}
		w.Header().Set("Content-Type", "application/json; charset=utf-8")
		w.Header().Set("Cache-Control", "no-store")
		_ = json.NewEncoder(w).Encode(map[string]any{"logs": h.Logs.Snapshot()})
	})

	mux.HandleFunc("/api/logs/clear", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "只支持 POST", http.StatusMethodNotAllowed)
			return
		}
		if h.Logs != nil {
			h.Logs.Clear()
		}
		w.WriteHeader(http.StatusNoContent)
	})
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
