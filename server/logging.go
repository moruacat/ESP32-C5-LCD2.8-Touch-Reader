package main

import (
	"log"
	"net/http"
	"strings"
	"time"
)

// statusRecorder 记住状态码和响应字节数，供日志中间件使用。
type statusRecorder struct {
	http.ResponseWriter
	status int
	bytes  int
}

func (r *statusRecorder) WriteHeader(code int) {
	r.status = code
	r.ResponseWriter.WriteHeader(code)
}

func (r *statusRecorder) Write(b []byte) (int, error) {
	n, err := r.ResponseWriter.Write(b)
	r.bytes += n
	return n, err
}

// withLogging 打印每个请求；但章节级请求太频繁（一本书几百次），
// 正常且不慢的就静默，只在出错或超过 500ms 时记录 —— 否则下载一本书会刷屏。
func withLogging(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		rec := &statusRecorder{ResponseWriter: w, status: http.StatusOK}

		next.ServeHTTP(rec, r)

		dur := time.Since(start)

		isChapter := strings.HasPrefix(r.URL.Path, "/api/books/") &&
			strings.Contains(r.URL.Path, "/chapter/")
		if isChapter && rec.status == http.StatusOK && dur < 500*time.Millisecond {
			return
		}

		log.Printf("%s %s → %d  %s  %d B  %s",
			r.Method, r.URL.RequestURI(), rec.status,
			dur.Round(time.Millisecond), rec.bytes, r.RemoteAddr)
	})
}
