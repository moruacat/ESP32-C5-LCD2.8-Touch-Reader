package fanqie

import (
	_ "embed"
	"encoding/json"
	"strings"
)

// charset.json 是番茄私用区(PUA)到真实汉字的固定映射表。
//
// 来源：番茄把正文里的常用字替换成 U+E3E8..U+E55C 区间的私用区字符，
// 再用自定义字体渲染（所以网页上看得见、复制出来是乱码）。
// 这套码位分配是全局固定的，不随书/字体变化，因此映射表一次内置即可。
//
// 生成方式：把字体字形渲染成图片再 OCR（社区项目 ckenkuo/fanqie-cdp-downloader），
// 实测 1183 个 PUA 字符全部还原、残留 0。
//
//go:embed charset.json
var charsetJSON []byte

// 两套码位范围：mode 0 是实测正确的那套，mode 1 用于极少数字体变体。
var codeRanges = [][2]rune{
	{0xE3E8, 0xE55B},
	{0xE3E9, 0xE55C},
}

var charset [][]string

func init() {
	if err := json.Unmarshal(charsetJSON, &charset); err != nil {
		panic("fanqie: charset.json 解析失败: " + err.Error())
	}
}

// Decode 把正文里的私用区字符还原成真实汉字。
//
// 先用 mode 0；如果还有残留（说明这本书用的是另一套字体），再补一次 mode 1。
func Decode(s string) string {
	out := decodeWith(s, 0)
	if puaCount(out) > 0 {
		out = decodeWith(out, 1)
	}
	return out
}

func decodeWith(s string, mode int) string {
	lo, hi := codeRanges[mode][0], codeRanges[mode][1]
	var b strings.Builder
	b.Grow(len(s))

	for _, r := range s {
		if r >= lo && r <= hi {
			bias := int(r - lo)
			if bias < len(charset[mode]) && charset[mode][bias] != "?" {
				b.WriteString(charset[mode][bias])
				continue
			}
		}
		b.WriteRune(r)
	}
	return b.String()
}

// puaCount 统计文本里还残留多少番茄私用区字符。
func puaCount(s string) int {
	n := 0
	for _, r := range s {
		for _, rg := range codeRanges {
			if r >= rg[0] && r <= rg[1] {
				n++
				break
			}
		}
	}
	return n
}
