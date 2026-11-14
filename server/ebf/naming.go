package ebf

import (
	"regexp"
	"strings"
	"unicode/utf8"
)

// illegalNameRe 对应 Python 的 _ILLEGAL_NAME_RE：Windows/FATFS 不接受的字符。
var illegalNameRe = regexp.MustCompile(`[<>:"/\\|?*\x00-\x1f]`)

// reservedNames 是 Windows 设备名，做文件夹名会出问题。
var reservedNames = map[string]bool{
	"CON": true, "PRN": true, "AUX": true, "NUL": true,
	"COM1": true, "COM2": true, "COM3": true, "COM4": true, "COM5": true,
	"COM6": true, "COM7": true, "COM8": true, "COM9": true,
	"LPT1": true, "LPT2": true, "LPT3": true, "LPT4": true, "LPT5": true,
	"LPT6": true, "LPT7": true, "LPT8": true, "LPT9": true,
}

// SafeDirName 把书名转成能当文件夹名的字符串（与 Python 版规则一致）。
func SafeDirName(title string) string {
	name := illegalNameRe.ReplaceAllString(title, "_")
	// \s+ → 单空格（对应 re.sub(r"\s+", " ")）
	name = strings.Join(strings.Fields(name), " ")
	name = strings.TrimSpace(name)
	name = strings.Trim(name, ".")
	name = strings.TrimSpace(name)

	if reservedNames[strings.ToUpper(name)] {
		name += "_"
	}

	// 按字符（不是字节）截断到 100
	if utf8.RuneCountInString(name) > 100 {
		runes := []rune(name)
		name = string(runes[:100])
	}
	if name == "" {
		return "book"
	}
	return name
}
