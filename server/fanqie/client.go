// Package fanqie 抓取番茄小说的公开内容并还原其字体混淆。
//
// 三个接口（都是公开端点，无需登录）：
//   - 搜索：novel.snssdk.com（官方免签名 App 搜索端点）
//   - 目录：fanqienovel.com/api/reader/directory/detail
//   - 正文：fanqienovel.com/reader/{itemId} 页面里的 window.__INITIAL_STATE__
//
// 正文返回的私用区字符由 charset.go 的映射表还原。
package fanqie

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"regexp"
	"strconv"
	"strings"
	"time"
)

const (
	searchAPI    = "https://novel.snssdk.com/api/novel/channel/homepage/search/search/v1/"
	searchAltAPI = "https://api-lf.fanqiesdk.com/api/novel/channel/homepage/search/search/v1/"
	directoryAPI = "https://fanqienovel.com/api/reader/directory/detail"
	readerPrefix = "https://fanqienovel.com/reader/"

	appUA = "com.dragon.read/68132 (Linux; U; Android 10; zh_CN; OnePlus11; Build/V291IR;tt-ok/3.12.13.4-tiktok)"
	webUA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"

	searchAID = "1967"
)

var (
	initialStateRe = regexp.MustCompile(`(?s)window\.__INITIAL_STATE__\s*=\s*(\{.*?\})\s*;`)
	paraRe         = regexp.MustCompile(`(?s)<p[^>]*>(.*?)</p>`)
	tagRe          = regexp.MustCompile(`<[^>]+>`)
)

// Client 是一个懒连接、无状态的抓取客户端。
type Client struct {
	HTTP *http.Client
}

// NewClient 默认 25 秒超时。
func NewClient() *Client {
	return &Client{HTTP: &http.Client{Timeout: 25 * time.Second}}
}

// SearchResult 是一条搜索结果。
type SearchResult struct {
	BookID   string
	Title    string
	Author   string
	Abstract string
	CoverURL string
	Score    string
}

// Chapter 是目录里的一章。
type Chapter struct {
	ItemID string
	Title  string
	Volume string
	Locked bool
}

// Book 汇总一本书的元信息。
type Book struct {
	BookID   string
	Title    string
	Author   string
	Abstract string
	CoverURL string
}

// BookInfo 从详情页读书名/作者/简介。
//
// 番茄详情页没有 og:novel:* 这类标签，元数据藏在 __INITIAL_STATE__ 的
// bookName/authorName 字段里；keywords 的第一个逗号前也是书名。
func (c *Client) BookInfo(bookID string) (*Book, error) {
	u := "https://fanqienovel.com/page/" + url.PathEscape(bookID)
	body, err := c.get(u, webUA, "")
	if err != nil {
		return nil, err
	}
	page := string(body)

	// 从 __INITIAL_STATE__ 里按 key 取字符串值
	pick := func(keys ...string) string {
		for _, k := range keys {
			re := regexp.MustCompile(`"` + k + `"\s*:\s*"((?:[^"\\]|\\.)*)"`)
			if m := re.FindStringSubmatch(page); len(m) == 2 {
				if s, err := strconv.Unquote(`"` + m[1] + `"`); err == nil {
					if t := strings.TrimSpace(s); t != "" {
						return t
					}
				}
			}
		}
		return ""
	}

	book := &Book{BookID: bookID}
	book.Title = pick("bookName", "book_name")
	book.Author = pick("authorName", "author_name")

	if book.Title == "" {
		// keywords 形如「书名,书名免费阅读,...,作者小说书名,...」
		if m := metaRe("keywords").FindStringSubmatch(page); len(m) == 2 {
			if i := strings.IndexAny(m[1], ",，"); i > 0 {
				book.Title = strings.TrimSpace(m[1][:i])
			}
		}
	}
	if book.Title == "" {
		// 最后退回 <title>「书名完整版在线免费阅读_...」
		if m := titleRe.FindStringSubmatch(page); len(m) == 2 {
			t := m[1]
			for _, cut := range []string{"完整版", "_", "-"} {
				if i := strings.Index(t, cut); i > 0 {
					t = t[:i]
				}
			}
			book.Title = strings.TrimSpace(t)
		}
	}
	if book.Title == "" {
		return nil, fmt.Errorf("详情页里没找到书名（bookId=%s）", bookID)
	}

	// 简介：meta description 前面常带「番茄小说提供xxx完整版在线免费阅读，」前缀
	if m := metaRe("description").FindStringSubmatch(page); len(m) == 2 {
		abs := strings.TrimSpace(m[1])
		if i := strings.Index(abs, "精彩小说尽在番茄小说网。"); i >= 0 {
			abs = strings.TrimSpace(abs[i+len("精彩小说尽在番茄小说网。"):])
		}
		book.Abstract = abs
	}
	return book, nil
}

// metaRe 匹配 <meta name="key" content="...">。
func metaRe(key string) *regexp.Regexp {
	return regexp.MustCompile(`<meta[^>]+name="` + regexp.QuoteMeta(key) + `"[^>]*content="([^"]*)"`)
}

var titleRe = regexp.MustCompile(`(?s)<title[^>]*>(.*?)</title>`)

// scoreText 把 score（数字或字符串）统一成展示文本。
func scoreText(v any) string {
	switch x := v.(type) {
	case float64:
		return fmt.Sprintf("%.1f", x)
	case string:
		return strings.TrimSpace(x)
	case nil:
		return ""
	default:
		return fmt.Sprint(x)
	}
}

func (c *Client) get(rawURL, ua, referer string) ([]byte, error) {	req, err := http.NewRequest(http.MethodGet, rawURL, nil)
	if err != nil {
		return nil, err
	}
	if ua != "" {
		req.Header.Set("User-Agent", ua)
	}
	if referer != "" {
		req.Header.Set("Referer", referer)
	}

	resp, err := c.HTTP.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("HTTP %d：%s", resp.StatusCode, rawURL)
	}
	return io.ReadAll(resp.Body)
}

// Search 按关键词搜索。offset 用于翻页，返回 (结果, 是否还有更多)。
func (c *Client) Search(keyword string, offset int) ([]SearchResult, bool, error) {
	var lastErr error
	for _, endpoint := range []string{searchAPI, searchAltAPI} {
		u := fmt.Sprintf("%s?aid=%s&offset=%d&q=%s", endpoint, searchAID, offset, url.QueryEscape(keyword))
		body, err := c.get(u, appUA, "")
		if err != nil {
			lastErr = err
			continue
		}

		var root struct {
			Code int `json:"code"`
			Data struct {
				HasMore bool `json:"has_more"`
				RetData []struct {
					BookID   json.Number `json:"book_id"`
					Title    string      `json:"title"`
					Author   string      `json:"author"`
					Abstract string      `json:"abstract"`
					ThumbURL string      `json:"thumb_url"`
					// score 有时是数字有时是字符串，交给运行时判断
					Score any `json:"score"`
				} `json:"ret_data"`
			} `json:"data"`
		}
		if err := json.Unmarshal(body, &root); err != nil {
			lastErr = err
			continue
		}
		if root.Code != 0 && root.Code != 200 {
			lastErr = fmt.Errorf("搜索接口返回 code=%d", root.Code)
			continue
		}

		out := make([]SearchResult, 0, len(root.Data.RetData))
		for _, item := range root.Data.RetData {
			out = append(out, SearchResult{
				BookID:   item.BookID.String(),
				Title:    strings.TrimSpace(item.Title),
				Author:   strings.TrimSpace(item.Author),
				Abstract: strings.TrimSpace(item.Abstract),
				CoverURL: item.ThumbURL,
				Score:    scoreText(item.Score),
			})
		}
		return out, root.Data.HasMore, nil
	}
	return nil, false, lastErr
}

// Directory 拉整本目录（含卷名，卷名挂在每章上）。
func (c *Client) Directory(bookID string) ([]Chapter, error) {
	u := directoryAPI + "?bookId=" + url.QueryEscape(bookID)
	body, err := c.get(u, webUA, "https://fanqienovel.com/page/"+bookID)
	if err != nil {
		return nil, err
	}

	var root struct {
		Code int `json:"code"`
		Data struct {
			ChapterListWithVolume [][]struct {
				ItemID        json.Number `json:"itemId"`
				Title         string      `json:"title"`
				VolumeName    string      `json:"volume_name"`
				IsChapterLock bool        `json:"isChapterLock"`
				NeedPay       int         `json:"needPay"`
			} `json:"chapterListWithVolume"`
		} `json:"data"`
	}
	if err := json.Unmarshal(body, &root); err != nil {
		return nil, fmt.Errorf("目录解析失败: %w", err)
	}

	var out []Chapter
	for _, volume := range root.Data.ChapterListWithVolume {
		for _, ch := range volume {
			out = append(out, Chapter{
				ItemID: ch.ItemID.String(),
				Title:  strings.TrimSpace(ch.Title),
				Volume: strings.TrimSpace(ch.VolumeName),
				Locked: ch.IsChapterLock || ch.NeedPay > 0,
			})
		}
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("目录为空（bookId=%s）", bookID)
	}
	return out, nil
}

// FetchChapter 拉一章正文，返回 (标题, 正文, 错误)。正文已做字体还原。
func (c *Client) FetchChapter(bookID, itemID string) (string, string, error) {
	referer := "https://fanqienovel.com/page/" + bookID
	body, err := c.get(readerPrefix+url.PathEscape(itemID), webUA, referer)
	if err != nil {
		return "", "", err
	}

	m := initialStateRe.FindSubmatch(body)
	if len(m) != 2 {
		return "", "", fmt.Errorf("阅读页里没有 __INITIAL_STATE__（itemId=%s）", itemID)
	}

	var state struct {
		Reader struct {
			ChapterData struct {
				Content       string `json:"content"`
				Title         string `json:"title"`
				IsChapterLock bool   `json:"isChapterLock"`
				NeedPay       int    `json:"needPay"`
			} `json:"chapterData"`
		} `json:"reader"`
	}
	if err := json.Unmarshal(m[1], &state); err != nil {
		return "", "", fmt.Errorf("阅读页状态解析失败: %w", err)
	}

	cd := state.Reader.ChapterData
	if cd.IsChapterLock || cd.NeedPay > 0 {
		return cd.Title, "", fmt.Errorf("章节已锁定或需付费")
	}
	if strings.TrimSpace(cd.Content) == "" {
		return cd.Title, "", fmt.Errorf("阅读页没有返回正文")
	}

	// content 是 HTML 片段：抽出 <p> 段落、去标签、还原字体混淆
	var paras []string
	for _, mm := range paraRe.FindAllStringSubmatch(cd.Content, -1) {
		text := strings.TrimSpace(tagRe.ReplaceAllString(mm[1], ""))
		if text == "" {
			continue
		}
		paras = append(paras, Decode(text))
	}
	if len(paras) == 0 {
		return cd.Title, "", fmt.Errorf("正文段落为空")
	}
	return cd.Title, strings.Join(paras, "\n"), nil
}
