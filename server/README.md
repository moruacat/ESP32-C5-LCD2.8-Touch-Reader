# ClockReader 书房服务端

局域网小说书库：EPUB 转换、番茄追更、Web 管理、设备自动发现。

> 方案与取舍见 [`docs/server_plan.md`](../docs/server_plan.md)。
> 当前进度：**阶段 A（骨架）**——服务能启动、能被设备发现、管理页可看书单；
> EPUB 转换与番茄追更尚未接入（上传会明确返回「转换器尚未接入」）。

---

## 编译

目标机是 **Intel Atom / i386 Linux**，没有 Go 也能跑：在本机交叉编译成单文件拷过去。

```bash
make cross        # 产出 clockreader-server-linux-386（约 11.5 MB）
```

原理：SQLite 用的是 **`modernc.org/sqlite`（纯 Go）**，所以 `CGO_ENABLED=0` 就能编 i386，
**不需要 i686 的 gcc 工具链**。

## 运行

```bash
./clockreader-server-linux-386
```

首次启动会自动生成 `config.json` 和 `data/`：

```json
{
  "http_addr": ":8080",
  "udp_port": 9527,
  "name": "ClockReader 书房",
  "data_dir": "./data"
}
```

| 目录/文件 | 用途 |
|---|---|
| `data/books.db` | SQLite（书单、章节索引、任务） |
| `data/books/<书名>.ebf/` | 转换后的 EBF 文件 |

## 设备发现

设备广播一行文本即可拿到服务地址：

```
设备 → 255.255.255.255:9527    CLOCKREADER?DISCOVER=1
服务端 → 单播                  CLOCKREADER!URL=http://:8080;NAME=ClockReader 书房
```

**URL 里主机名是空的**——服务端通常不知道自己在局域网里的 IP（多网卡更麻烦），
所以设备要**用收到回复的那个源地址**来补全主机名。

## 接口

### 管理页

`http://<服务器 IP>:8080/` — 书单列表（上传在阶段 B 接入）。

### 设备 API

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/ping` | 探活，返回 `{"ok":true,"ver":1}` |
| GET | `/api/books` | 书单（id/title/author/chapters/bytes/hash） |
| GET | `/api/books/{id}/meta` | 详情 + 内容哈希（设备据此判断要不要更新） |
| GET | `/api/books/{id}/data` | `data.ebfl` |
| GET | `/api/books/{id}/chapters` | `chapters.ebfl`（大纲） |
| GET | `/api/books/{id}/cover` | `cover.raw`（RGB565 小端） |
| GET | `/api/books/{id}/chapter/N` | 第 N 章正文 |

**按 EBF 文件粒度下发**：设备拿到的就是最终要落盘的文件，**零解压、零解析**，
断点续传按文件天然成立（重下同名文件覆盖即可）。

## 目录结构

```
server/
├── main.go          入口：HTTP + UDP
├── config.go        config.json 读写
├── db/db.go         SQLite schema 与 DAO
├── discover/udp.go  UDP 广播发现
├── api/device.go    设备 API
├── web/handler.go   管理页
└── Makefile         交叉编译
```

## 待接入（阶段 B/C）

- [ ] EPUB → EBF 转换（排版：段首两个全角空格 + 段间空行）
- [ ] 上传/批量/覆盖更新（按书名+作者匹配，哈希对比决定增删改）
- [ ] 番茄追更（`go-novel-dl` 子进程，`--start` 增量）
