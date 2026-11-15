package fanqie

// SM3（国密散列）—— Go 标准库与 x/crypto 都没有，这里自己实现。
// 移植自 naiyQAQ/fanqie-assistant 的油猴脚本（其算法与 GB/T 32905-2016 一致）。

const sm3BlockSize = 64

var sm3IV = [8]uint32{
	0x7380166f, 0x4914b2b9, 0x172442d7, 0xda8a0600,
	0xa96f30bc, 0x163138aa, 0xe38dee4d, 0xb0fb0e4e,
}

func sm3Rotl(x uint32, n uint) uint32 {
	n &= 31
	return x<<n | x>>(32-n)
}

func sm3P0(x uint32) uint32 { return x ^ sm3Rotl(x, 9) ^ sm3Rotl(x, 17) }
func sm3P1(x uint32) uint32 { return x ^ sm3Rotl(x, 15) ^ sm3Rotl(x, 23) }

// sm3Sum 计算 SM3 摘要（32 字节）。
func sm3Sum(data []byte) [32]byte {
	// 填充：0x80 + 0x00... + 64 位比特长度
	msgLen := len(data)
	paddedLen := (msgLen + 9 + sm3BlockSize - 1) / sm3BlockSize * sm3BlockSize
	msg := make([]byte, paddedLen)
	copy(msg, data)
	msg[msgLen] = 0x80

	bitLen := uint64(msgLen) * 8
	for i := 0; i < 8; i++ {
		msg[paddedLen-1-i] = byte(bitLen >> (uint(i) * 8))
	}

	state := sm3IV
	var w [68]uint32

	for block := 0; block < paddedLen/sm3BlockSize; block++ {
		base := block * sm3BlockSize
		for i := 0; i < 16; i++ {
			o := base + i*4
			w[i] = uint32(msg[o])<<24 | uint32(msg[o+1])<<16 | uint32(msg[o+2])<<8 | uint32(msg[o+3])
		}
		for i := 16; i < 68; i++ {
			w[i] = sm3P1(w[i-16]^w[i-9]^sm3Rotl(w[i-3], 15)) ^ sm3Rotl(w[i-13], 7) ^ w[i-6]
		}

		a, b, c, d := state[0], state[1], state[2], state[3]
		e, f, g, h := state[4], state[5], state[6], state[7]

		for j := 0; j < 64; j++ {
			var t uint32 = 0x79cc4519
			if j > 15 {
				t = 0x7a879d8a
			}
			ss1 := sm3Rotl(sm3Rotl(a, 12)+e+sm3Rotl(t, uint(j)), 7)
			ss2 := ss1 ^ sm3Rotl(a, 12)

			var ff, gg uint32
			if j <= 15 {
				ff = a ^ b ^ c
				gg = e ^ f ^ g
			} else {
				ff = (a & b) | (a & c) | (b & c)
				gg = (e & f) | (^e & g)
			}

			tt1 := ff + d + ss2 + (w[j] ^ w[j+4])
			tt2 := gg + h + ss1 + w[j]

			d = c
			c = sm3Rotl(b, 9)
			b = a
			a = tt1
			h = g
			g = sm3Rotl(f, 19)
			f = e
			e = sm3P0(tt2)
		}

		state[0] ^= a
		state[1] ^= b
		state[2] ^= c
		state[3] ^= d
		state[4] ^= e
		state[5] ^= f
		state[6] ^= g
		state[7] ^= h
	}

	var out [32]byte
	for i, v := range state {
		out[i*4] = byte(v >> 24)
		out[i*4+1] = byte(v >> 16)
		out[i*4+2] = byte(v >> 8)
		out[i*4+3] = byte(v)
	}
	return out
}
