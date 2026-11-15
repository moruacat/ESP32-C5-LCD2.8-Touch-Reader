package main

import (
	"log"
	"net/http"
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

// withLogging 打印每个请求（方法、路径、状态码、耗时、字节数、来源）。
//
// 章节级请求也照打不误 —— 需要能从日志里看出"设备到底拉了哪几章"。
// 批量接口一次 20 章只产生一条日志，所以不会像逐章那样刷屏。
func withLogging(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		rec := &statusRecorder{ResponseWriter: w, status: http.StatusOK}

		next.ServeHTTP(rec, r)

		log.Printf("%s %s → %d  %s  %d B  %s",
			r.Method, r.URL.RequestURI(), rec.status,
			time.Since(start).Round(time.Millisecond), rec.bytes, r.RemoteAddr)
	})
}
