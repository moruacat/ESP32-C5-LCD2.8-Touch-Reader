package ebf

import (
	"bytes"
	"image"
	_ "image/gif"
	_ "image/jpeg"
	_ "image/png"
	"math"

	"golang.org/x/image/draw"
)

// 与固件/转换脚本对齐的封面上限
const (
	CoverMaxW = 240
	CoverMaxH = 320
)

// ConvertCover 把任意图片字节转成 EBF 封面（等比缩放到 ≤240x320 的 RGB565 小端）。
// 给非 EPUB 来源（番茄封面）复用。
func ConvertCover(raw []byte) (*Cover, bool) {
	w, h, data, ok := convertCover(raw)
	if !ok {
		return nil, false
	}
	return &Cover{W: w, H: h, Data: data}, true
}

// convertCover 把图片字节等比缩放到不超过 240x320，转成 RGB565 小端。
//
// 只缩小不放大 —— 小图放大会糊，而且固件本来就按原始尺寸显示。
// 注意：缩放算法与 Python 版（Pillow 的 thumbnail）不同，像素不会逐字节一致，
// 这是可接受的：封面只用于展示，不影响章节文本的比对。
func convertCover(raw []byte) (w, h int, data []byte, ok bool) {
	img, _, err := image.Decode(bytes.NewReader(raw))
	if err != nil {
		return 0, 0, nil, false
	}

	src := img.Bounds()
	iw, ih := src.Dx(), src.Dy()
	if iw <= 0 || ih <= 0 {
		return 0, 0, nil, false
	}

	scale := math.Min(float64(CoverMaxW)/float64(iw), float64(CoverMaxH)/float64(ih))
	if scale > 1 {
		scale = 1 // 不放大
	}
	nw := int(float64(iw) * scale)
	nh := int(float64(ih) * scale)
	if nw < 1 {
		nw = 1
	}
	if nh < 1 {
		nh = 1
	}

	dst := image.NewRGBA(image.Rect(0, 0, nw, nh))
	draw.CatmullRom.Scale(dst, dst.Bounds(), img, src, draw.Over, nil)

	out := make([]byte, nw*nh*2)
	for y := 0; y < nh; y++ {
		for x := 0; x < nw; x++ {
			r, g, b, _ := dst.At(x, y).RGBA()
			// 8 位分量 → RGB565
			rv := uint16(r >> 8)
			gv := uint16(g >> 8)
			bv := uint16(b >> 8)
			v := ((rv & 0xF8) << 8) | ((gv & 0xFC) << 3) | (bv >> 3)
			// 低字节在前（LVGL RGB565 的内存布局）
			idx := (y*nw + x) * 2
			out[idx] = byte(v & 0xFF)
			out[idx+1] = byte(v >> 8)
		}
	}
	return nw, nh, out, true
}
