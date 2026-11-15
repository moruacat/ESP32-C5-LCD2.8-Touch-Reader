package web

import (
	"bytes"
	"image"
	"image/color"
	"image/png"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// registerCover 挂封面缩略图路由：GET /cover?id=N
//
// cover.raw 是固件用的 RGB565 小端裸数据，浏览器认不了，这里转成 PNG。
// 尺寸从 data.ebfl 的 <cover width="W" height="H" .../> 读。
func (h *Handler) registerCover(mux *http.ServeMux) {
	mux.HandleFunc("/cover", func(w http.ResponseWriter, r *http.Request) {
		id, err := strconv.ParseInt(r.URL.Query().Get("id"), 10, 64)
		if err != nil || id <= 0 {
			http.NotFound(w, r)
			return
		}

		book, err := h.Store.GetBook(id)
		if err != nil || book.EbfDir == "" {
			http.NotFound(w, r)
			return
		}

		raw, err := os.ReadFile(filepath.Join(book.EbfDir, "cover.raw"))
		if err != nil {
			http.NotFound(w, r)
			return
		}

		bw, bh := readCoverDims(filepath.Join(book.EbfDir, "data.ebfl"))
		if bw <= 0 || bh <= 0 || len(raw) < bw*bh*2 {
			http.NotFound(w, r)
			return
		}

		img := image.NewRGBA(image.Rect(0, 0, bw, bh))
		for i := 0; i < bw*bh; i++ {
			v := uint16(raw[2*i]) | uint16(raw[2*i+1])<<8
			r5 := uint8((v >> 11) & 0x1F)
			g6 := uint8((v >> 5) & 0x3F)
			b5 := uint8(v & 0x1F)
			img.Set(i%bw, i/bw, color.RGBA{
				R: r5<<3 | r5>>2,
				G: g6<<2 | g6>>4,
				B: b5<<3 | b5>>2,
				A: 255,
			})
		}

		var buf bytes.Buffer
		if err := png.Encode(&buf, img); err != nil {
			http.Error(w, "封面编码失败", http.StatusInternalServerError)
			return
		}

		w.Header().Set("Content-Type", "image/png")
		w.Header().Set("Cache-Control", "public, max-age=86400")
		_, _ = w.Write(buf.Bytes())
	})
}

// readCoverDims 从 data.ebfl 头部读封面尺寸。
func readCoverDims(dataPath string) (w, h int) {
	f, err := os.Open(dataPath)
	if err != nil {
		return 0, 0
	}
	defer f.Close()

	head := make([]byte, 2048)
	n, _ := f.Read(head)
	s := string(head[:n])

	i := strings.Index(s, "<cover ")
	if i < 0 {
		return 0, 0
	}
	tag := s[i:]
	if j := strings.Index(tag, "/>"); j >= 0 {
		tag = tag[:j]
	}
	return attrInt(tag, "width"), attrInt(tag, "height")
}

func attrInt(tag, key string) int {
	needle := key + `="`
	i := strings.Index(tag, needle)
	if i < 0 {
		return 0
	}
	rest := tag[i+len(needle):]
	j := strings.Index(rest, `"`)
	if j < 0 {
		return 0
	}
	n, _ := strconv.Atoi(rest[:j])
	return n
}
