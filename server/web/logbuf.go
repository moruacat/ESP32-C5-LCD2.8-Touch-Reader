package web

import (
	"strings"
	"sync"
	"time"
)

const logKeep = 1200

// LogBuffer 记住最近若干行日志，供管理页轮询显示。
// 实现 io.Writer，直接挂到 log.SetOutput 上即可。
type LogBuffer struct {
	mu    sync.Mutex
	lines []logLine
}

type logLine struct {
	At   string `json:"at"`
	Text string `json:"text"`
}

// NewLogBuffer 创建一个空缓冲。
func NewLogBuffer() *LogBuffer { return &LogBuffer{} }

// Write 把一行日志追加进去（log 包每次 printf 是完整一行）。
func (b *LogBuffer) Write(p []byte) (int, error) {
	text := strings.TrimRight(string(p), "\r\n")
	if text == "" {
		return len(p), nil
	}

	b.mu.Lock()
	defer b.mu.Unlock()

	b.lines = append(b.lines, logLine{
		At:   time.Now().Format("15:04:05"),
		Text: text,
	})
	if len(b.lines) > logKeep {
		b.lines = b.lines[len(b.lines)-logKeep:]
	}
	return len(p), nil
}

// Snapshot 取全部缓存行（新→旧顺序由前端处理）。
func (b *LogBuffer) Snapshot() []logLine {
	b.mu.Lock()
	defer b.mu.Unlock()

	out := make([]logLine, len(b.lines))
	copy(out, b.lines)
	return out
}

// Clear 清空。
func (b *LogBuffer) Clear() {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.lines = nil
}
