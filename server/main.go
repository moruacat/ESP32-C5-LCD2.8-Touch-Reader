// clockreader-server：局域网小说书库服务端。
//
// 部署：交叉编译出 i386 单文件，放到 Atom 服务器上直接跑：
//
//	CGO_ENABLED=0 GOOS=linux GOARCH=386 go build -trimpath -ldflags="-s -w" -o clockreader-server .
//
// 启动后：
//   - HTTP 管理页 + 设备 API 监听 config.json 里的 http_addr（默认 :8080）
//   - UDP 发现服务监听 udp_port（默认 9527），设备广播即可拿到服务地址
//
// 详见 docs/server_plan.md。
package main

import (
	"flag"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"time"

	"clockreader-server/api"
	"clockreader-server/db"
	"clockreader-server/discover"
	"clockreader-server/importer"
	"clockreader-server/web"
)

func main() {
	cfgPath := flag.String("config", "config.json", "配置文件路径")
	flag.Parse()

	log.SetFlags(log.LstdFlags | log.Lmsgprefix)
	log.SetPrefix("[server] ")

	cfg, err := loadConfig(*cfgPath)
	if err != nil {
		log.Fatalf("加载配置失败: %v", err)
	}
	if err := os.MkdirAll(cfg.DataDir, 0o755); err != nil {
		log.Fatalf("创建数据目录失败: %v", err)
	}

	store, err := db.Open(filepath.Join(cfg.DataDir, "books.db"))
	if err != nil {
		log.Fatalf("打开数据库失败: %v", err)
	}
	defer store.Close()

	// 日志同时进 stderr 和内存缓冲：管理页底部轮询 /api/logs 显示
	logBuf := web.NewLogBuffer()
	log.SetOutput(io.MultiWriter(os.Stderr, logBuf))

	// UDP 发现：让设备不用手填 IP
	go func() {
		if err := discover.Run(cfg.UDPPort, cfg.Name, cfg.HTTPAddr); err != nil {
			log.Printf("UDP 发现服务不可用: %v", err)
		}
	}()

	mux := http.NewServeMux()

	dev := &api.Handler{Store: store, EbfRoot: filepath.Join(cfg.DataDir, "books")}
	dev.Register(mux)

	imp := &importer.Importer{
		Store:   store,
		EbfRoot: filepath.Join(cfg.DataDir, "books"),
	}
	webHandler := &web.Handler{
		Store: store, Importer: imp.Import, Logs: logBuf, Addr: cfg.HTTPAddr,
	}
	webHandler.Register(mux, cfg.HTTPAddr)

	srv := &http.Server{
		Addr:              cfg.HTTPAddr,
		Handler:           withLogging(mux),
		ReadHeaderTimeout: 5 * time.Second,
	}

	log.Printf("书库服务启动：HTTP %s，数据目录 %s", cfg.HTTPAddr, cfg.DataDir)
	if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatalf("HTTP 服务退出: %v", err)
	}
}
