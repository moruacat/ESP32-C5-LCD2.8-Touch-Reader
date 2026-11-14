# 在线书库服务端方案（Go · i386-Linux · HTTP）

> 状态：**计划阶段**，尚未实现。
> 目标：设备通过局域网从自建服务端拉取小说；服务端负责 EPUB 转换、番茄追更、Web 管理。

---

## 0. 结论摘要

| 决策 | 选择 | 理由 |
|---|---|---|
| 服务端语言 | **Go 1.22+，CGO_ENABLED=0** | i386 交叉编译零工具链 |
| SQLite 驱动 | **`modernc.org/sqlite`（纯 Go）** | `mattn/go-sqlite3` 需要 i686 gcc，交叉编译麻烦 |
| 传输协议 | **纯 HTTP** | 按需求；局域网内无需 TLS |
| 书库分发格式 | **章节级 HTTP API** | 设备零解压、可断点续传，最省内存 |
| 服务器发现 | **UDP 广播 + 手动 IP 兜底** | 设备端 ~2 KB 成本；mDNS 已移除且更重 |
| 番茄下载 | **`guohuiyuan/go-novel-dl`（Go，原生支持番茄）** | Go 项目，CLI/Web/EPUB 导出齐全，子进程或 fork 都行 |
| 正文排版 | **段首缩进 2 全角空格 + 段间空行** | 对齐用户提供的样本 EPUB 视觉风格 |

---

## 1. 服务器发现（关键问题）

### 1.1 方案对比

| 方案 | 设备端成本 | 服务端成本 | 适用 | 结论 |
|---|---|---|---|---|
| **手动填 IP** | 0 | 0 | 任何环境 | **必做兜底** |
| **UDP 广播** | ~2 KB 任务 + 约 200 行 | 约 50 行 | 同一网段 | **推荐（自动发现）** |
| mDNS | 10–20 KB（组件已移除） | 系统 avahi | 同一网段 | 不做（内存代价大） |
| 固定公网域名 | 0 | 需 DNS/公网 IP | 公网 | 可选（填 URL 即可） |

**推荐组合：启动先用 NVS 里的地址；为空则 UDP 广播发现，3 秒超时重试 3 次；失败则提示手动填写。**

### 1.2 UDP 发现协议

```
设备 → 广播 255.255.255.255:9527
  "CLOCKREADER?DISCOVER=1"

服务端 → 单播回复发送方
  "CLOCKREADER!URL=http://192.168.1.50:8080;NAME=书房;VER=1"
```

- 纯 ASCII、固定前缀，解析不用 JSON；
- 设备解析到 `URL` 后写入 NVS（`wifi 服务器地址`），下次直接连；
- 服务端也可同时监听多个网卡，回复时用收到请求的本机地址。

### 1.3 设备端何时触发

| 时机 | 行为 |
|---|---|
| 开机 WiFi 连上后 | 有缓存地址 → 直接探活（`GET /api/ping`，2 秒超时） |
| 探活失败 | 触发一次广播发现 |
| 设置页「重新发现」 | 手动触发 |
| 设置页手动填 | 写 NVS，跳过发现 |

---

## 2. 服务端架构

### 2.1 目录结构

```
server/
├── main.go            入口：加载配置、启动 HTTP + UDP
├── config.go          JSON 配置（端口、库路径、番茄开关）
├── db/                SQLite：schema 与 DAO
│   ├── schema.sql
│   └── db.go
├── api/               设备 API（/api/...）
│   └── device.go
├── web/               管理界面（HTML 模板 + 内嵌静态资源）
│   ├── handler.go
│   └── templates/
├── ebf/               EPUB → EBF 转换
│   ├── epub.go        解析 EPUB
│   ├── ebf.go         写 EBF 目录
│   └── cover.go       封面缩放 → RGB565 LE
├── fanqie/            番茄下载（子进程）
│   └── runner.go
├── discover/          UDP 发现
│   └── udp.go
└── static/
```

### 2.2 SQLite 表

```sql
CREATE TABLE books (
    id            INTEGER PRIMARY KEY,
    title         TEXT NOT NULL,
    author        TEXT,
    fanqie_id     TEXT,              -- 番茄 book_id，追更用
    chapters      INTEGER DEFAULT 0,
    bytes         INTEGER DEFAULT 0,
    content_hash  TEXT,              -- 章节内容整体哈希，用于判断是否更新
    source        TEXT,              -- upload / fanqie
    ebf_dir       TEXT NOT NULL,     -- 服务端存储路径
    updated_at    TEXT
);

CREATE TABLE chapters (
    book_id   INTEGER,
    no        INTEGER,               -- 从 1 连续
    name      TEXT,
    bytes     INTEGER,
    hash      TEXT,
    PRIMARY KEY (book_id, no)
);

CREATE TABLE jobs (
    id         INTEGER PRIMARY KEY,
    kind       TEXT,                 -- convert / fanqie / merge
    status     TEXT,                 -- pending/running/done/failed
    progress   INTEGER,              -- 0..100
    message    TEXT,
    created_at TEXT
);
```

`chapters` 表是**覆盖更新**的核心：按 `(book_id, no)` 对比哈希就知道哪些章变了、哪些是新增。

### 2.3 交叉编译

```bash
# 纯 Go，无 CGO
CGO_ENABLED=0 GOOS=linux GOARCH=386 go build -trimpath -ldflags="-s -w" -o clockreader-server .
```

产物约 10–15 MB（含 SQLite），单文件部署。运行时需要写权限目录（`data/` 存 SQLite 和 EBF 文件）。

---

## 3. EPUB → EBF 转换（Go 重写）

### 3.1 EBF 格式（以 `tools/epub2ebf/epub2ebf.py` 为准）

```
<书名>.ebf/
├── data.ebfl        XML：<book chapters="N">、title/author/intro/cover/volumes
├── chapters.ebfl    一行一章名，行序 = 章号（大纲）
├── chapter.N.ebfl   XML 头（章名/卷名）+ 空行 + 正文
└── cover.raw        RGB565 小端，等比缩放到不超过 240×320
```

**硬约束（设备端固件决定）：**

| 项 | 上限 | 来源 |
|---|---|---|
| 书名 | 180 字节 | `SRV_BOOK_NAME_MAX = 192` |
| 作者 | 44 字节 | `SRV_BOOK_AUTHOR_MAX = 48` |
| 单章正文 | 4 MB | `SRV_READER_TEXT_MAX` |
| 封面 | 240×320 / 1 MB | `EBF_COVER_MAX` |
| 章号 | 从 1 **连续** | `srv_library` 数文件 |

### 3.2 Go 实现要点

- EPUB 是 ZIP：`archive/zip` 读 `META-INF/container.xml` → OPF → spine 顺序；
- 正文抽离：`golang.org/x/net/html`，跳过 `script/style/head/svg/math`，块级标签产换行（对照 Python 版的 `_SKIP_TAGS`）；
- 封面：`image/jpeg`/`image/png` 解码 → `golang.org/x/image/draw` 等比缩放（**不放大**）→ RGB565 **小端**；
- 章节名清理：去首尾空白、限长（字节截断按 UTF-8 边界）。

**验收方式**：拿现有 `tools/epub2ebf` 的 Python 输出做**逐字节比对**（同一本 EPUB 转换后 `diff -r`），确保固件解析行为完全一致。

### 3.3 排版规范（用户指定：对齐样本 EPUB）

样本：`不错！猫猫我就是隐藏BOSS.epub`（其他工具生成，用户认可其观感）。它的样子是：

```html
<h1>第1章 我变成了……谁？</h1>
<p>【前方即将进入另一个世界……】</p>
<p>【放只噬元兽在这吃脑子……】</p>
```

```css
p { text-indent: 2em; margin: 0 0 .8em 0; line-height: 1.5; }
```

即：**段首缩进 2 字符 + 段间距 0.8em（视觉空行）**。EBF 是纯文本，要复刻这个观感必须落到字符上：

| 特征 | EBF 里的写法 | 说明 |
|---|---|---|
| 段首缩进 | 每段开头加 **两个全角空格 `　　`** | 设备端阅读器不自动缩进（已确认），只能靠文本 |
| 段间空行 | 段间插一个空行（`\n\n`），即现有 `clean_text(blank_line=True)` | 小屏上对应 CSS 的 `margin:.8em` |
| 章节标题 | 章名单独一行，正文**不重复**标题 | 复用现有 `drop_dup_title()` 逻辑 |
| 简介页 | `aux_*.xhtml` 里含「书名：/作者：/简介：」的 → 提取成 `<intro>` | **不要**当章节 |
| 目录页 | `table-of-contents.html` / `toc.ncx` / `nav` → 跳过 | 否则会多出一章「目录」 |
| 图片 | 跳过（EBF 无图片位，样本里也只有段评配图） | 保持纯文本 |

**现有 `tools/epub2ebf/epub2ebf.py` 与目标的差距**（需要在新实现里补）：

1. `blank_line` 默认 `False`——样本风格应为 `True`；
2. 不生成段首缩进；
3. 按 spine 切章时会把简介页、目录页也当成章（样本的 `spine` 里就在 `chapter_00001` 之前有 `aux_00000` 和 `table-of-contents.html`），需要用文件名/内容规则筛掉。

---

## 4. 覆盖更新（追更）

### 4.1 两种来源

| 来源 | 匹配依据 | 行为 |
|---|---|---|
| EPUB 上传 | 书名 + 作者（可手动指定覆盖哪本） | 解析后与 `chapters` 表对比 |
| 番茄追更 | `fanqie_id` | 拉章节列表 → 只抓新增 |

### 4.2 合并策略

1. 按 `(title, author)` 查已有书；
2. 新 EPUB 解析出章节序列；
3. **按章号逐个对比 `chapters.hash`**：
   - 章号存在且哈希相同 → 跳过；
   - 章号存在但哈希不同 → 覆盖（作者修订）；
   - 章号超出旧章数 → 追加；
4. 重写 `data.ebfl`（`chapters=N`）、`chapters.ebfl`、新增/变更的 `chapter.N.ebfl`；
5. **不动** `cover.raw`（除非新 EPUB 带封面且用户勾选替换）。

**设备端阅读进度**按（书路径 + 章号）存在 `srv_progress`，追加章节不影响进度；若发生"章节删除/重排"（新 EPUB 少了章），需在 Web 上提示并让用户选择「完整替换」。

### 4.3 增量下发

设备拉取时只取变化的部分：

```
GET /api/books/{id}/meta
→ {"id":1,"title":"...","chapters":1200,"hash":"abc...","updated_at":"..."}

设备对比本地 data.ebfl 的 chapters 数 → 从 N 开始拉
GET /api/books/{id}/chapter/1201
```

---

## 5. 番茄下载集成（`guohuiyuan/go-novel-dl`）

### 5.1 为什么不用 fanqie-dl

`linzj/fanqie-dl` 是**纯 Rust 的交互式 CLI**：运行后从 stdin 读 `book_id`，只输出 TXT 到 `downloads/<书名>/`，
**没有 HTTP API、没有非交互参数**——自动追更时无法可靠判断"完成"或"新增了哪几章"。

`guohuiyuan/go-novel-dl` 则完全对得上需求：

| 能力 | 说明 |
|---|---|
| **Go 实现** | 与本服务端同语言，必要时可以直接 fork 改 |
| **原生支持番茄** | 搜索走官方免签名端点 `novel.snssdk.com`，目录/正文走 `fanqienovel.com` |
| **CLI** | `novel-dl download <book_id> --site fanqie`、`export <book_id>` |
| **章节范围** | `--start / --end`，追更时只抓新增章 |
| **Web / API** | 自带管理界面（默认 18089） |
| **导出** | txt / html / epub |
| **部署** | Docker 镜像（NAS 上有现成教程） |

### 5.2 集成方式（三选一）

| 方式 | 做法 | 优点 | 缺点 |
|---|---|---|---|
| **A. 子进程 CLI**（先做这个） | `os/exec` 调 `novel-dl download <id> --site fanqie` + `export` | 零耦合、随它升级 | 只能靠输出目录判断结果 |
| **B. 调它的 Web API** | 本地起 `novel-dl web --port 18089`，服务端 HTTP 调 | 有结构化进度 | 依赖它内部 API 稳定性 |
| **C. fork 引入 Go 包** | 直接 import 它的下载器包 | 最干净 | 要维护 fork |

**先做 A**：追更时用 `--start <本地已有章数+1>` 只抓新增章；成功与否看导出文件是否生成。
如果 A 的进度判断不够用，再评估 B/C。

### 5.3 追更定时任务

- 服务端 Go ticker（默认每 6 小时，可配）；
- 遍历 `books WHERE fanqie_id IS NOT NULL AND auto_update=1`；
- 调 `novel-dl export <id>` 或 `download --start N --end 99999` 拿新章；
- 解析导出的 TXT/EPUB → 走第 4 节的哈希合并 → 重转 EBF；
- 每本之间随机延迟 10–30 秒，避免触发番茄限流；
- 结果写 `jobs` 表，Web 上可见进度与失败原因。

### 5.4 版式一致性

go-novel-dl 导出的 TXT 自带章节标题和段落。转换前统一按第 3.3 节的排版规范处理
（**段首缩进 2 全角空格 + 段间空行**），保证「上传的 EPUB」和「追更来的内容」在设备上观感一致。

---

## 6. 设备端改动

### 6.1 新增模块

| 模块 | 职责 | 内存 |
|---|---|---|
| `srv_server` | 服务器地址（NVS）、UDP 发现、探活 | ~2 KB 任务栈 |
| `srv_catalog` | 书单拉取、分章下载、写 SD | 流式 4 KB + HTTP 会话（PSRAM） |

### 6.2 API 设计（设备侧）

```
GET  /api/ping                  → {"ok":true,"ver":1}
GET  /api/books                 → 书单（id/title/author/chapters/bytes）
GET  /api/books/{id}/meta       → 详情 + 章节数 + 内容哈希
GET  /api/books/{id}/cover      → cover.raw（RGB565 LE）
GET  /api/books/{id}/chapters   → chapters.ebfl（大纲）
GET  /api/books/{id}/data       → data.ebfl（元数据）
GET  /api/books/{id}/chapter/N  → 单章正文（chapter.N.ebfl）
```

**为什么按文件粒度**：设备端拿到的就是 EBF 的原始文件，**直接写 SD 即可**，不需要任何解析/解压/重排——下载逻辑最简单，内存最省，断点续传按文件天然成立。

### 6.3 UI

- **设置 → 在线书库**：服务器地址（自动发现/手动填）、测试连接、同步书单；
- **书架**：多一个「在线」标签，显示服务器书单 + 「下载/更新」按钮；已下载的书显示「已同步」或「有 N 章更新」；
- 下载进度复用 OTA 那套全屏进度层思路（OTA 已移除，代码可以借鉴）。

### 6.4 内存预算（已评估）

| 项 | PSRAM | 内部 RAM |
|---|---|---|
| UDP 发现 | ~2 KB | 少量 |
| HTTP 下载（含 TLS 缓冲） | ~40 KB | ~5 KB |
| SD 写入缓冲 | 4 KB | — |
| **合计** | **~50 KB / 2.6 MB** | **~5 KB / 30+ KB** |

**纯 HTTP 更省**（无 TLS）。下载期间建议暂停小智 AI，避免内部 RAM 峰值。

---

## 7. 分阶段计划

| 阶段 | 内容 | 产出 | 估时 |
|---|---|---|---|
| **A** | 服务端骨架：Go 项目、SQLite、Web 管理（书单/上传）、UDP 发现、i386 编译脚本 | 能跑起来的管理页 + 发现服务 | 2 天 |
| **B** | EPUB→EBF 转换（Go）+ 上传/批量 + 与 Python 版逐字节比对 | 转换正确性可验证 | 3 天 |
| **C** | 覆盖更新（哈希对比/合并）+ 番茄追更（子进程，必要时 fork） | 能自动追更 | 2–3 天 |
| **D** | 设备端 `srv_server` + `srv_catalog` + 设置/书架 UI | 设备能发现并下载 | 3 天 |
| **E** | 联调、异常路径（断网/半截文件/重复下载）、文档 | 可用版本 | 1–2 天 |

**合计约 11–13 天**（单人；番茄集成若需 fork 会多 1–2 天）。

---

## 8. 风险清单

| 风险 | 影响 | 缓解 |
|---|---|---|
| **go-novel-dl 的 CLI/导出行为变化** | 追更解析失败 | 优先用 `--start/--end` 增量；必要时固定版本或 fork |
| 番茄 API 变更/限流 | 下载失败 | 延迟 + 重试 + Web 报错；保留手动上传兜底 |
| i386 交叉编译 | SQLite 需要 CGO | 用纯 Go 驱动，`CGO_ENABLED=0` |
| EPUB 章节重排/删章 | 覆盖更新错乱 | 哈希对比 + 用户确认「完整替换」 |
| 简介/目录页被当成章节 | 书里多出「简介」「目录」两章 | 按文件名与「书名：/作者：」关键词筛掉 |
| 设备 SD 写入慢（几 MB） | 下载耗时长 | 分章下载 + 进度 + 断点续传 |
| 下载与 AI 并发 | 内部 RAM 不足 | 下载前暂停 AI |
| 服务端 IP 变化 | 设备连不上 | UDP 重新发现 + 设置页手动改 |
| 书名/作者字节超限 | 固件截断 | 转换时按 UTF-8 边界截断（与 Python 版一致） |

---

## 9. 待确认

1. **服务端部署位置**：局域网 NAS / 树莓派（UDP 广播可用），还是公网 VPS（需手填域名）？
2. **在线书架入口**：放在书架页多一个标签，还是设置里单独一页？
3. **正文排版**：段首缩进用「两个全角空格」还是「一个制表位」？（样本是 CSS 的 `text-indent:2em`，转成纯文本时这两种视觉最接近，前者更稳妥）
4. **追更频率**：默认 6 小时一次是否合适？
