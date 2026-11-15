package fanqie

// App 接口（reading.snssdk.com）需要三个签名头：x-argus / x-ladon / x-khronos。
// 算法来自社区逆向（naiyQAQ/fanqie-assistant 的油猴脚本），这里用 Go 重写：
//   - SIMON / SPECK 两个轻量块密码（纯位运算）
//   - 手工 protobuf 编码
//   - x-argus：protobuf → PKCS7 → SIMON → XOR → AES-CBC
//   - x-ladon：时间戳拼接 → MD5 派生密钥 → SPECK → base64
//
// 这些算法不依赖浏览器环境，可以直接在服务端跑。

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/md5"
	"encoding/base64"
	"encoding/binary"
	"fmt"
	"strconv"
	"strings"
	"time"
)

// ---------------------------------------------------------------------------
// protobuf 最小写入器
// ---------------------------------------------------------------------------

const (
	wireVarint = 0
	wireBytes  = 2
	wireFixed  = 5
)

type pbWriter struct{ buf []byte }

func (w *pbWriter) varint(v uint64) *pbWriter {
	for v >= 0x80 {
		w.buf = append(w.buf, byte(v)|0x80)
		v >>= 7
	}
	w.buf = append(w.buf, byte(v))
	return w
}

func (w *pbWriter) key(field, wire int) *pbWriter {
	return w.varint(uint64(field)<<3 | uint64(wire))
}

func (w *pbWriter) Varint(field int, v uint32) *pbWriter {
	return w.key(field, wireVarint).varint(uint64(v))
}

func (w *pbWriter) Bytes(field int, data []byte) *pbWriter {
	w.key(field, wireBytes).varint(uint64(len(data)))
	w.buf = append(w.buf, data...)
	return w
}

func (w *pbWriter) String(field int, s string) *pbWriter {
	return w.Bytes(field, []byte(s))
}

func (w *pbWriter) Message(field int, build func(*pbWriter)) *pbWriter {
	sub := &pbWriter{}
	build(sub)
	return w.Bytes(field, sub.buf)
}

// ---------------------------------------------------------------------------
// SIMON（72 轮，Z4 序列）—— 用于 x-argus
// ---------------------------------------------------------------------------

const (
	simonRounds = 72
	simonZ4     = 0x3dc94c3a046d678b
)

func simonKeyExpansion(key [4]uint64) [simonRounds]uint64 {
	var k [simonRounds]uint64
	copy(k[:4], key[:])
	for i := 4; i < simonRounds; i++ {
		tmp := rotl64(k[i-1], 64-3) ^ k[i-3]
		tmp ^= rotl64(tmp, 64-1)
		zbit := uint64((simonZ4 >> uint((i-4)%62)) & 1)
		k[i] = ^k[i-4] ^ tmp ^ zbit ^ 3
	}
	return k
}

// simonEncrypt 加密一个 128 位块（x=低 64 位，y=高 64 位）。
func simonEncrypt(plain [2]uint64, key [4]uint64) [2]uint64 {
	k := simonKeyExpansion(key)
	x, y := plain[0], plain[1]
	for i := 0; i < simonRounds; i++ {
		tmp := y
		f := (rotl64(y, 1) & rotl64(y, 8)) ^ rotl64(y, 2)
		y = x ^ f ^ k[i]
		x = tmp
	}
	return [2]uint64{x, y}
}

func rotl64(v uint64, n uint) uint64 {
	n &= 63
	return v<<n | v>>(64-n)
}

func rotr64(v uint64, n uint) uint64 {
	n &= 63
	return v<<(64-n) | v>>n
}

// ---------------------------------------------------------------------------
// SPECK（34 轮）—— 用于 x-ladon
// ---------------------------------------------------------------------------

const (
	speckRounds = 34
	speckAlpha  = 8
	speckBeta   = 3
)

func speckKeySchedule(key []byte) []uint64 {
	// key 32 字节 → 4 个 64 位小端字
	ks := make([]uint64, 0, speckRounds)
	l := make([]uint64, 0, 4)
	for i := 0; i < 4; i++ {
		l = append(l, binary.LittleEndian.Uint64(key[i*8:]))
	}
	ks = append(ks, l[0])
	for x := 0; x < speckRounds-1; x++ {
		rsX := rotl64(l[x+1], 64-speckAlpha) + ks[x]
		newX := uint64(x) ^ rsX
		lsY := rotl64(ks[x], 64-speckBeta)
		l = append(l, newX)
		ks = append(ks, newX^lsY)
	}
	return ks
}

func speckEncryptBlock(ks []uint64, block []byte, out []byte) {
	y := binary.LittleEndian.Uint64(block[0:])
	x := binary.LittleEndian.Uint64(block[8:])
	for _, k := range ks {
		rsX := rotl64(x, 64-speckAlpha) + y
		x = k ^ rsX
		lsY := rotl64(y, 64-speckBeta)
		y = x ^ lsY
	}
	binary.LittleEndian.PutUint64(out[0:], y)
	binary.LittleEndian.PutUint64(out[8:], x)
}

func pkcs7Pad(data []byte, size int) []byte {
	pad := size - len(data)%size
	out := make([]byte, len(data)+pad)
	copy(out, data)
	for i := len(data); i < len(out); i++ {
		out[i] = byte(pad)
	}
	return out
}

func speckEncrypt(key, plaintext []byte) []byte {
	padded := pkcs7Pad(plaintext, 16)
	ks := speckKeySchedule(key)
	out := make([]byte, len(padded))
	for i := 0; i < len(padded); i += 16 {
		speckEncryptBlock(ks, padded[i:i+16], out[i:i+16])
	}
	return out
}

// ---------------------------------------------------------------------------
// x-argus / x-ladon / x-khronos
// ---------------------------------------------------------------------------

// appConfig 是签名用的固定参数（来自脚本的 defaultUnidbgConfig）。
type appConfig struct {
	signKey       []byte
	aid           string
	licenseID     string
	sdkVersion    string
	sdkVersionInt uint32
	callType      uint32
}

var defaultAppConfig = appConfig{
	// 脚本里的 signKey（十六进制）
	signKey:       mustHex("ac1adaae95a7af94a5114ab3b3a97dd80050aa0a39314c40528caec95256c28c"),
	aid:           "1967",
	licenseID:     "1611921764",
	sdkVersion:    "v04.04.05-ov-android",
	sdkVersionInt: 134744640,
	callType:      738,
}

// appDevice 是 App 侧的设备标识（游客设备）。
// 与脚本默认值一致；不同设备标识可能影响接口的可用性。
type appDevice struct {
	InstallID  string
	DeviceID   string
	DeviceType string
}

var defaultAppDevice = appDevice{
	InstallID:  "2187355326270644",
	DeviceID:   "2187355326004404",
	DeviceType: "P30",
}

func mustHex(s string) []byte {
	out := make([]byte, len(s)/2)
	for i := 0; i < len(out); i++ {
		var b byte
		_, err := fmt.Sscanf(s[i*2:i*2+2], "%02x", &b)
		if err != nil {
			panic("fanqie: 非法十六进制串")
		}
		out[i] = b
	}
	return out
}

// sm3Prefix6 取 SM3 的前 6 字节。
func sm3Prefix6(data []byte) []byte {
	sum := sm3Sum(data)
	return sum[:6]
}

// buildArgusProtobuf 组装 x-argus 的 protobuf 明文。
func buildArgusProtobuf(query, xssStub string, ts int64, cfg appConfig, dev appDevice) []byte {
	params := parseQuery(query)
	deviceID := params["device_id"]
	versionName := params["version_name"]

	var stubBytes []byte
	if xssStub != "" {
		stubBytes = decodeStub(xssStub)
	} else {
		stubBytes = make([]byte, 16)
	}
	bodyHash := sm3Prefix6(stubBytes)

	var queryBytes []byte
	if query != "" {
		queryBytes = []byte(query)
	} else {
		queryBytes = make([]byte, 16)
	}
	queryHash := sm3Prefix6(queryBytes)

	randVal := uint32(time.Now().UnixNano()&0x7fffffff) + 1

	w := &pbWriter{}
	w.Varint(1, 538970409*2).
		Varint(2, 2).
		Varint(3, randVal).
		String(4, cfg.aid).
		String(5, deviceID).
		String(6, cfg.licenseID).
		String(7, versionName).
		String(8, cfg.sdkVersion).
		Varint(9, cfg.sdkVersionInt).
		Bytes(10, make([]byte, 8)).
		Varint(11, 0).
		Varint(12, uint32(ts*2)).
		Bytes(13, bodyHash).
		Bytes(14, queryHash).
		Message(15, func(sub *pbWriter) {
			sub.Varint(1, 1).Varint(2, 1).Varint(3, 1).Varint(7, 3348294860)
		}).
		String(16, "").
		String(20, "none").
		Varint(21, cfg.callType).
		Message(23, func(sub *pbWriter) {
			sub.String(1, "NX551J").Varint(2, 8196).Varint(4, 2162219008)
		}).
		Varint(25, 2)
	return w.buf
}

// decodeStub 把 32 位十六进制字符串解成 16 字节。
func decodeStub(s string) []byte {
	out := make([]byte, 16)
	if len(s) < 32 {
		return out
	}
	for i := 0; i < 16; i++ {
		var b byte
		_, _ = fmt.Sscanf(s[i*2:i*2+2], "%02x", &b)
		out[i] = b
	}
	return out
}

// parseQuery 把 "a=1&b=2" 解析成 map。
func parseQuery(q string) map[string]string {
	out := make(map[string]string)
	for _, pair := range strings.Split(q, "&") {
		if pair == "" {
			continue
		}
		kv := strings.SplitN(pair, "=", 2)
		if len(kv) != 2 {
			continue
		}
		out[kv[0]] = kv[1]
	}
	return out
}

// getArgus 生成 x-argus。
func getArgus(query, xssStub string, ts int64, cfg appConfig, dev appDevice) (string, error) {
	if len(cfg.signKey) != 32 {
		return "", fmt.Errorf("signKey 长度必须是 32，实际 %d", len(cfg.signKey))
	}

	protobuf := buildArgusProtobuf(query, xssStub, ts, cfg, dev)
	padded := pkcs7Pad(protobuf, 16)

	lowRand := []byte{242, 129}
	highRand := []byte{97, 111}

	sm3Input := make([]byte, 0, len(cfg.signKey)*2+len(lowRand)+len(highRand))
	sm3Input = append(sm3Input, cfg.signKey...)
	sm3Input = append(sm3Input, lowRand...)
	sm3Input = append(sm3Input, highRand...)
	sm3Input = append(sm3Input, cfg.signKey...)

	digest := sm3Sum(sm3Input)

	simonKey := [4]uint64{
		binary.LittleEndian.Uint64(digest[0:]),
		binary.LittleEndian.Uint64(digest[8:]),
		binary.LittleEndian.Uint64(digest[16:]),
		binary.LittleEndian.Uint64(digest[24:]),
	}

	encrypted := make([]byte, len(padded))
	for off := 0; off < len(padded); off += 16 {
		plain := [2]uint64{
			binary.LittleEndian.Uint64(padded[off:]),
			binary.LittleEndian.Uint64(padded[off+8:]),
		}
		low, high := simonEncrypt(plain, simonKey)[0], simonEncrypt(plain, simonKey)[1]
		binary.LittleEndian.PutUint64(encrypted[off:], low)
		binary.LittleEndian.PutUint64(encrypted[off+8:], high)
	}

	// XOR 前缀 + 反序
	xorPrefix := []byte{242, 247, 252, 255, 242, 247, 252, 255}
	data := make([]byte, 0, len(xorPrefix)+len(encrypted))
	data = append(data, xorPrefix...)
	data = append(data, encrypted...)
	for i := len(xorPrefix); i < len(data); i++ {
		data[i] ^= data[i%8]
	}
	reverseBytes(data)

	header := []byte{166, 110, 173, 159, 119, 1, 208, 12, 24}
	plaintext := make([]byte, 0, len(header)+len(data)+len(highRand))
	plaintext = append(plaintext, header...)
	plaintext = append(plaintext, data...)
	plaintext = append(plaintext, highRand...)

	// AES-CBC：key = MD5(signKey[:16])，iv = MD5(signKey[16:])
	// 注意：脚本用 WebCrypto 的 subtle.encrypt，它会自动做 PKCS7 填充；
	// Go 的 cipher.BlockMode 不填充，必须自己补，否则长度不是 16 的倍数会 panic。
	keyMD5 := md5.Sum(cfg.signKey[:16])
	ivMD5 := md5.Sum(cfg.signKey[16:])

	block, err := aes.NewCipher(keyMD5[:])
	if err != nil {
		return "", err
	}
	paddedPlain := pkcs7Pad(plaintext, 16)
	ciphertext := make([]byte, len(paddedPlain))
	cipher.NewCBCEncrypter(block, ivMD5[:]).CryptBlocks(ciphertext, paddedPlain)

	out := make([]byte, 0, len(lowRand)+len(ciphertext))
	out = append(out, lowRand...)
	out = append(out, ciphertext...)
	return base64.StdEncoding.EncodeToString(out), nil
}

// getLadon 生成 x-ladon。
func getLadon(ts int64, cfg appConfig) (string, error) {
	randomBytes := []byte{byte(time.Now().UnixNano()), byte(time.Now().UnixNano() >> 8), 0x5a, 0x3c}

	plaintext := []byte(fmt.Sprintf("%d-%s-%s", ts, cfg.licenseID, cfg.aid))

	// key = MD5(randomBytes || aid) 的十六进制字符串
	h := md5.New()
	h.Write(randomBytes)
	h.Write([]byte(cfg.aid))
	key := []byte(fmt.Sprintf("%x", h.Sum(nil)))

	encrypted := speckEncrypt(key, plaintext)

	out := make([]byte, 0, len(randomBytes)+len(encrypted))
	out = append(out, randomBytes...)
	out = append(out, encrypted...)
	return base64.StdEncoding.EncodeToString(out), nil
}

// signHeaders 生成三个签名头。
func signHeaders(query, xssStub string, ts int64) (map[string]string, error) {
	argus, err := getArgus(query, xssStub, ts, defaultAppConfig, defaultAppDevice)
	if err != nil {
		return nil, err
	}
	ladon, err := getLadon(ts, defaultAppConfig)
	if err != nil {
		return nil, err
	}
	return map[string]string{
		"x-argus":   argus,
		"x-ladon":   ladon,
		"x-khronos": strconv.FormatInt(ts, 10),
	}, nil
}

func reverseBytes(b []byte) {
	for i, j := 0, len(b)-1; i < j; i, j = i+1, j-1 {
		b[i], b[j] = b[j], b[i]
	}
}
