// Package ebf 把 EPUB 转成固件读的 EBF 目录。
//
// 格式（与 tools/epub2ebf/epub2ebf.py 保持一致，固件按此解析）：
//
//	<书名>.ebf/
//	├── data.ebfl       XML 元数据：<book chapters="N"> + title/author/intro/cover/volumes
//	├── chapters.ebfl   一行一章名，行序 = 章号
//	├── chapter.N.ebfl  XML 首部（章名/卷名）+ 空行 + 正文原文
//	└── cover.raw       RGB565 小端，等比缩放到不超过 240x320
package ebf

import (
	"archive/zip"
	"encoding/xml"
	"fmt"
	"io"
	"net/url"
	"path"
	"strings"
)

// ManifestItem 是 OPF manifest 里的一项。
type ManifestItem struct {
	ID    string
	Href  string
	Path  string // zip 内真实路径（大小写已对齐）；空表示书里没有
	Media string
	Props []string
}

// TocNode 是目录树节点：有子节点表示分卷，叶子表示章节。
type TocNode struct {
	Title    string
	Href     string
	Path     string // 解析后的 zip 路径；空表示指向的文件不在书里
	Children []TocNode
}

// Book 是解析后的 EPUB。
type Book struct {
	Zip   *zip.ReadCloser
	names map[string]string // 小写名 → 真实名（EPUB 里大小写常不一致）

	OPFDir     string
	Title      string
	Author     string
	Intro      string
	CoverMeta  string // <meta name="cover" content="...">
	Manifest   map[string]*ManifestItem
	Spine      []*ManifestItem
	spinePos   map[string]int
}

// OpenBook 打开并解析 EPUB 的 OPF（container.xml → .opf）。
func OpenBook(epubPath string) (*Book, error) {
	zr, err := zip.OpenReader(epubPath)
	if err != nil {
		return nil, fmt.Errorf("不是有效的 EPUB（zip 打不开）：%w", err)
	}

	b := &Book{
		Zip:      zr,
		names:    make(map[string]string, len(zr.File)),
		Manifest: make(map[string]*ManifestItem),
		spinePos: make(map[string]int),
	}
	for _, f := range zr.File {
		b.names[strings.ToLower(f.Name)] = f.Name
	}

	opf, err := b.findOPF()
	if err != nil {
		zr.Close()
		return nil, err
	}
	b.OPFDir = path.Dir(opf)
	if err := b.parseOPF(opf); err != nil {
		zr.Close()
		return nil, err
	}
	if len(b.Spine) == 0 {
		zr.Close()
		return nil, fmt.Errorf("OPF 里没有 spine，无法确定正文顺序")
	}
	for i, item := range b.Spine {
		if _, ok := b.spinePos[item.Path]; !ok {
			b.spinePos[item.Path] = i
		}
	}
	return b, nil
}

// Close 关闭底层 zip。
func (b *Book) Close() error { return b.Zip.Close() }

// real 按小写名找 zip 里的真实条目名。
func (b *Book) real(name string) string {
	return b.names[strings.ToLower(name)]
}

// Read 读一个书内文件（大小写不敏感）。
func (b *Book) Read(name string) ([]byte, error) {
	real := b.real(name)
	if real == "" {
		return nil, fmt.Errorf("EPUB 里找不到文件：%s", name)
	}
	for _, f := range b.Zip.File {
		if f.Name != real {
			continue
		}
		rc, err := f.Open()
		if err != nil {
			return nil, err
		}
		defer rc.Close()
		return io.ReadAll(rc)
	}
	return nil, fmt.Errorf("EPUB 里找不到文件：%s", name)
}

// findOPF 先从 container.xml 找，失败则退回扫描 .opf。
func (b *Book) findOPF() (string, error) {
	if raw, err := b.Read("META-INF/container.xml"); err == nil {
		var c struct {
			Rootfiles []struct {
				FullPath string `xml:"full-path,attr"`
			} `xml:"rootfiles>rootfile"`
		}
		if xml.Unmarshal(raw, &c) == nil {
			for _, rf := range c.Rootfiles {
				if real := b.real(rf.FullPath); real != "" {
					return real, nil
				}
			}
		}
	}
	for _, f := range b.Zip.File {
		if strings.HasSuffix(strings.ToLower(f.Name), ".opf") {
			return f.Name, nil
		}
	}
	return "", fmt.Errorf("找不到 OPF 文件，这可能不是 EPUB")
}

// opfPackage 只声明我们关心的字段。
type opfPackage struct {
	Metadata struct {
		Title       string `xml:"title"`
		Creator     string `xml:"creator"`
		Description string `xml:"description"`
		Metas       []struct {
			Name    string `xml:"name,attr"`
			Content string `xml:"content,attr"`
		} `xml:"meta"`
	} `xml:"metadata"`
	Manifest struct {
		Items []struct {
			ID         string `xml:"id,attr"`
			Href       string `xml:"href,attr"`
			MediaType  string `xml:"media-type,attr"`
			Properties string `xml:"properties,attr"`
		} `xml:"item"`
	} `xml:"manifest"`
	Spine struct {
		ItemRefs []struct {
			IDRef string `xml:"idref,attr"`
		} `xml:"itemref"`
	} `xml:"spine"`
}

func (b *Book) parseOPF(opfPath string) error {
	raw, err := b.Read(opfPath)
	if err != nil {
		return err
	}
	var pkg opfPackage
	if err := xml.Unmarshal(raw, &pkg); err != nil {
		return fmt.Errorf("OPF 解析失败：%w", err)
	}

	// dc:title 这类带命名空间的前缀，encoding/xml 的 Local 已经去掉了
	b.Title = strings.TrimSpace(pkg.Metadata.Title)
	b.Author = strings.TrimSpace(pkg.Metadata.Creator)
	b.Intro = pkg.Metadata.Description
	for _, m := range pkg.Metadata.Metas {
		if strings.EqualFold(m.Name, "cover") {
			b.CoverMeta = m.Content
		}
	}

	for _, it := range pkg.Manifest.Items {
		if it.ID == "" || it.Href == "" {
			continue
		}
		item := &ManifestItem{
			ID:    it.ID,
			Href:  it.Href,
			Path:  b.real(resolveHref(b.OPFDir, it.Href)),
			Media: it.MediaType,
		}
		if it.Properties != "" {
			item.Props = strings.Fields(it.Properties)
		}
		b.Manifest[it.ID] = item
	}

	for _, ref := range pkg.Spine.ItemRefs {
		if item, ok := b.Manifest[ref.IDRef]; ok && item.Path != "" {
			b.Spine = append(b.Spine, item)
		}
	}
	return nil
}

// resolveHref 把相对 href 解析成 zip 内的绝对路径（去 fragment、URL 解码）。
func resolveHref(baseDir, href string) string {
	if i := strings.IndexByte(href, '#'); i >= 0 {
		href = href[:i]
	}
	href = strings.ReplaceAll(href, "\\", "/")
	if dec, err := url.PathUnescape(href); err == nil {
		href = dec
	}
	if href == "" {
		return ""
	}
	joined := path.Clean(path.Join(baseDir, href))
	return strings.TrimPrefix(joined, "/")
}
