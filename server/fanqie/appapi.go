package fanqie

// App 接口（reading.snssdk.com）的调用与章节解密。
//
// 与网页版相比，App 接口能拿到"手机版里能看到"的完整章节 ——
// 网页端对未登录/未授权用户只下发试读片段，App 端的权限规则不同。
// 请求需要 x-argus / x-ladon / x-khronos 三个签名（见 appsign.go），
// 返回的正文是 AES-CBC 密文，密钥要先用 /crypt/registerkey 换取。

import (
	"bytes"
	"compress/gzip"
	"crypto/aes"
	"crypto/cipher"
	"crypto/md5"
	"crypto/rand"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	appBaseURL = "https://reading.snssdk.com/reading"
	appUserAgent      = "com.dragon.read"

	// 设备密钥交换用的固定 16 字节
	sharedKeyHex = "ac25c67ddd8f38c1b37a2348828e222e"
)

// appClient 持有 key_info 缓存（换一次能用较久）。
type appClient struct {
	http *http.Client

	mu      sync.Mutex
	key     []byte
	keyVer  int
}

func newAppClient() *appClient {
	return &appClient{http: &http.Client{Timeout: 25 * time.Second}}
}

// buildAppQuery 组装 App 接口的公共参数。
func buildAppQuery(extra map[string]string) string {
	q := url.Values{}
	q.Set("iid", defaultAppDevice.InstallID)
	q.Set("device_id", defaultAppDevice.DeviceID)
	q.Set("ac", "wifi")
	q.Set("channel", "43536163a")
	q.Set("aid", "1967")
	q.Set("app_name", "novelapp")
	q.Set("version_code", "70132")
	q.Set("version_name", "7.0.1.32")
	q.Set("device_platform", "android")
	q.Set("os", "android")
	q.Set("ssmix", "a")
	q.Set("os_version", "10")
	q.Set("device_type", defaultAppDevice.DeviceType)
	q.Set("device_brand", "realme")
	q.Set("update_version_code", "70132")
	q.Set("manifest_version_code", "70132")
	for k, v := range extra {
		q.Set(k, v)
	}
	return q.Encode()
}

// appGet 发一个带签名的 GET，返回响应体。
func (c *appClient) appGet(path string, extra map[string]string) ([]byte, error) {
	query := buildAppQuery(extra)
	full := appBaseURL + path + "?" + query

	ts := time.Now().Unix()
	headers, err := signHeaders(query, "", ts)
	if err != nil {
		return nil, err
	}

	req, err := http.NewRequest(http.MethodGet, full, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", appUserAgent)
	for k, v := range headers {
		req.Header.Set(k, v)
	}
	req.Header.Set("x-ss-req-ticket", strconv.FormatInt(ts*1000, 10))

	resp, err := c.http.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	body, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK {
		head := body
		if len(head) > 200 {
			head = head[:200]
		}
		return nil, fmt.Errorf("App 接口 HTTP %d：%.200s", resp.StatusCode, string(head))
	}
	if len(body) == 0 {
		return nil, fmt.Errorf("App 接口返回空响应（签名可能未通过）")
	}
	return body, nil
}

// registerKey 向 /crypt/registerkey 换章节解密密钥。
func (c *appClient) registerKey() error {
	// 请求体：AES-CBC(sharedKey, iv=随机16字符, data=reverseHex(deviceID)[:8])
	iv := []byte(randomString(16))
	devBytes := reverseHexBytes(defaultAppDevice.DeviceID)
	if len(devBytes) > 8 {
		devBytes = devBytes[:8]
	}

	block, err := aes.NewCipher(mustHex(sharedKeyHex))
	if err != nil {
		return err
	}
	// WebCrypto 会自动 PKCS7 填充，Go 不会 —— 这里手动补
	paddedDev := pkcs7Pad(devBytes, 16)
	ciphertext := make([]byte, len(paddedDev))
	cipher.NewCBCEncrypter(block, iv).CryptBlocks(ciphertext, paddedDev)

	payload := append(append([]byte{}, iv...), ciphertext...)
	body := fmt.Sprintf(`{"content":%q}`, base64.StdEncoding.EncodeToString(payload))

	query := buildAppQuery(nil)
	full := appBaseURL + "/crypt/registerkey?" + query

	ts := time.Now().Unix()
	// POST 的 x-ss-stub 是 body 的 MD5
	xssStub := fmt.Sprintf("%x", md5.Sum([]byte(body)))
	headers, err := signHeaders(query, xssStub, ts)
	if err != nil {
		return err
	}

	req, err := http.NewRequest(http.MethodPost, full, strings.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("User-Agent", appUserAgent)
	req.Header.Set("Content-Type", "application/json; charset=utf-8")
	req.Header.Set("X-SS-STUB", xssStub)
	req.Header.Set("x-ss-req-ticket", strconv.FormatInt(ts*1000, 10))
	for k, v := range headers {
		req.Header.Set(k, v)
	}

	resp, err := c.http.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()

	raw, err := io.ReadAll(resp.Body)
	if err != nil {
		return err
	}

	var parsed struct {
		Data struct {
			Key    string `json:"key"`
			KeyVer int    `json:"keyver"`
		} `json:"data"`
	}
	if err := json.Unmarshal(raw, &parsed); err != nil {
		return fmt.Errorf("registerkey 响应解析失败: %w (body: %.200s)", err, string(raw))
	}
	if parsed.Data.Key == "" {
		return fmt.Errorf("registerkey 没有返回 key (body: %.200s)", string(raw))
	}

	// 响应里的 key 也是 AES-CBC(sharedKey, iv, data)
	decoded, err := base64.StdEncoding.DecodeString(parsed.Data.Key)
	if err != nil || len(decoded) < 32 {
		return fmt.Errorf("registerkey 的 key 解码失败")
	}
	iv2, data2 := decoded[:16], decoded[16:]

	block2, err := aes.NewCipher(mustHex(sharedKeyHex))
	if err != nil {
		return err
	}
	out := make([]byte, len(data2))
	cipher.NewCBCDecrypter(block2, iv2).CryptBlocks(out, data2)

	c.mu.Lock()
	c.key = out
	c.keyVer = parsed.Data.KeyVer
	c.mu.Unlock()
	return nil
}

// ensureKey 保证有可用密钥。
func (c *appClient) ensureKey(expectedVer int) error {
	c.mu.Lock()
	have := c.key != nil
	ver := c.keyVer
	c.mu.Unlock()

	if have && (expectedVer == 0 || expectedVer == ver) {
		return nil
	}
	return c.registerKey()
}

// chapterPayload 是 /reader/full/v 返回的章节数据（只取用到的字段）。
type chapterPayload struct {
	Content        string `json:"content"`
	Title          string `json:"title"`
	KeyVersion     int    `json:"key_version"`
	CompressStatus int    `json:"compress_status"`
	BookID         string `json:"book_id"`
	ItemID         string `json:"item_id"`
}

// fetchChapter 拉一章并解密，返回 (标题, 正文)。
func (c *appClient) fetchChapter(itemID string) (string, string, error) {
	if err := c.ensureKey(0); err != nil {
		return "", "", err
	}

	raw, err := c.appGet("/reader/full/v", map[string]string{
		"item_id":  itemID,
		"req_type": "1",
	})
	if err != nil {
		return "", "", err
	}

	var resp struct {
		Data *chapterPayload `json:"data"`
	}
	if err := json.Unmarshal(raw, &resp); err != nil {
		return "", "", fmt.Errorf("章节响应解析失败: %w", err)
	}
	if resp.Data == nil {
		return "", "", fmt.Errorf("章节响应没有 data")
	}
	ch := resp.Data

	// 密钥版本对不上 → 重新注册再试一次
	if ch.Content == "Invalid" || (ch.KeyVersion != 0 && ch.KeyVersion != c.currentKeyVer()) {
		if err := c.ensureKey(ch.KeyVersion); err != nil {
			return "", "", err
		}
		raw, err = c.appGet("/reader/full/v", map[string]string{
			"item_id":  itemID,
			"req_type": "1",
		})
		if err != nil {
			return "", "", err
		}
		if err := json.Unmarshal(raw, &resp); err != nil || resp.Data == nil {
			return "", "", fmt.Errorf("章节重试解析失败")
		}
		ch = resp.Data
	}

	plain, err := c.decrypt(ch.Content, ch.CompressStatus)
	if err != nil {
		return ch.Title, "", err
	}
	return ch.Title, plain, nil
}

func (c *appClient) currentKeyVer() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.keyVer
}

// decrypt 解一章正文：base64 → AES-CBC →（可选）gzip。
func (c *appClient) decrypt(encrypted string, compressStatus int) (string, error) {
	if strings.TrimSpace(encrypted) == "" {
		return "", fmt.Errorf("正文为空")
	}
	buf, err := base64.StdEncoding.DecodeString(encrypted)
	if err != nil {
		return "", fmt.Errorf("正文 base64 解码失败: %w", err)
	}
	if len(buf) < 32 {
		return "", fmt.Errorf("正文密文过短")
	}
	iv, data := buf[:16], buf[16:]

	c.mu.Lock()
	key := c.key
	c.mu.Unlock()
	if key == nil {
		return "", fmt.Errorf("还没有解密密钥")
	}

	block, err := aes.NewCipher(key)
	if err != nil {
		return "", err
	}
	out := make([]byte, len(data))
	cipher.NewCBCDecrypter(block, iv).CryptBlocks(out, data)

	if compressStatus == 1 {
		zr, err := gzip.NewReader(bytes.NewReader(out))
		if err != nil {
			return "", fmt.Errorf("gzip 解压失败: %w", err)
		}
		defer zr.Close()
		out, err = io.ReadAll(zr)
		if err != nil {
			return "", err
		}
	}

	// 去掉 PKCS7 填充
	if n := len(out); n > 0 {
		pad := int(out[n-1])
		if pad > 0 && pad <= 16 && pad <= n {
			out = out[:n-pad]
		}
	}
	return string(out), nil
}

// appCatalog 用 App 接口拉目录（比网页接口更全，且带字数/更新时间）。
func (c *appClient) appCatalog(bookID string) ([]Chapter, error) {
	raw, err := c.appGet("/bookapi/directory/all_items/v", map[string]string{
		"book_id": bookID,
	})
	if err != nil {
		return nil, err
	}

	var resp struct {
		Code int `json:"code"`
		Data struct {
			ItemDataList []struct {
				ItemID     json.Number `json:"item_id"`
				Title      string      `json:"title"`
				VolumeName string      `json:"volume_name"`
			} `json:"item_data_list"`
		} `json:"data"`
	}
	if err := json.Unmarshal(raw, &resp); err != nil {
		return nil, fmt.Errorf("目录解析失败: %w", err)
	}
	if len(resp.Data.ItemDataList) == 0 {
		return nil, fmt.Errorf("App 目录为空（bookId=%s）", bookID)
	}

	out := make([]Chapter, 0, len(resp.Data.ItemDataList))
	for _, it := range resp.Data.ItemDataList {
		out = append(out, Chapter{
			ItemID: it.ItemID.String(),
			Title:  strings.TrimSpace(it.Title),
			Volume: strings.TrimSpace(it.VolumeName),
		})
	}
	return out, nil
}

// randomString 生成 n 个字母数字字符。
func randomString(n int) string {
	const chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
	buf := make([]byte, n)
	if _, err := rand.Read(buf); err != nil {
		for i := range buf {
			buf[i] = chars[i%len(chars)]
		}
		return string(buf)
	}
	for i := range buf {
		buf[i] = chars[int(buf[i])%len(chars)]
	}
	return string(buf)
}

// reverseHexBytes 把十进制数字串按十六进制反转（deviceID → 字节）。
//
// 脚本里是 reverseHex(BigInt(deviceId).toString(16).padStart(32,"0"))，
// 这里等价实现：转成 32 位十六进制后按字节反序。
func reverseHexBytes(deviceID string) []byte {
	n, err := strconv.ParseUint(deviceID, 10, 64)
	if err != nil {
		return make([]byte, 8)
	}
	hexStr := fmt.Sprintf("%016x", n)
	be := make([]byte, 0, 32)
	for i := 0; i < len(hexStr); i += 2 {
		var b byte
		_, _ = fmt.Sscanf(hexStr[i:i+2], "%02x", &b)
		be = append(be, b)
	}
	// 反序
	out := make([]byte, len(be))
	for i := range be {
		out[i] = be[len(be)-1-i]
	}
	return out
}
