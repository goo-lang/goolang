// Vendored from Go 1.26 math/bits (src/math/bits/bits.go), verbatim.
// This is a curated SUBSET — the first functions proven to compile and run
// under Goo's frontend. It grows as more of the upstream file is unblocked by
// frontend features; nothing here is hand-adapted (the vendoring rule is
// upstream-verbatim, never edit real Go source).
package bits

// Bit-manipulation masks (from OnesCount/ReverseBytes in upstream bits.go).
const m0 = 0x5555555555555555 // 01010101 ...
const m1 = 0x3333333333333333 // 00110011 ...
const m2 = 0x0f0f0f0f0f0f0f0f // 00001111 ...
const m3 = 0x00ff00ff00ff00ff // etc.
const m4 = 0x0000ffff0000ffff

// OnesCount64 returns the number of one bits ("population count") in x.
func OnesCount64(x uint64) int {
	// Implementation: Parallel summing of adjacent bits.
	// See "Hacker's Delight", Chap. 5: Counting Bits.
	const m = 1<<64 - 1
	x = x>>1&(m0&m) + x&(m0&m)
	x = x>>2&(m1&m) + x&(m1&m)
	x = (x>>4 + x) & (m2 & m)
	x += x >> 8
	x += x >> 16
	x += x >> 32
	return int(x) & (1<<7 - 1)
}

// ReverseBytes16 returns the value of x with its bytes in reversed order.
//
// This function's execution time does not depend on the inputs.
func ReverseBytes16(x uint16) uint16 {
	return x>>8 | x<<8
}

// ReverseBytes32 returns the value of x with its bytes in reversed order.
//
// This function's execution time does not depend on the inputs.
func ReverseBytes32(x uint32) uint32 {
	const m = 1<<32 - 1
	x = x>>8&(m3&m) | x&(m3&m)<<8
	return x>>16 | x<<16
}

// ReverseBytes64 returns the value of x with its bytes in reversed order.
//
// This function's execution time does not depend on the inputs.
func ReverseBytes64(x uint64) uint64 {
	const m = 1<<64 - 1
	x = x>>8&(m3&m) | x&(m3&m)<<8
	x = x>>16&(m4&m) | x&(m4&m)<<16
	return x>>32 | x<<32
}

// RotateLeft8 returns the value of x rotated left by (k mod 8) bits.
// To rotate x right by k bits, call RotateLeft8(x, -k).
//
// This function's execution time does not depend on the inputs.
func RotateLeft8(x uint8, k int) uint8 {
	const n = 8
	s := uint(k) & (n - 1)
	return x<<s | x>>(n-s)
}

// RotateLeft16 returns the value of x rotated left by (k mod 16) bits.
// To rotate x right by k bits, call RotateLeft16(x, -k).
//
// This function's execution time does not depend on the inputs.
func RotateLeft16(x uint16, k int) uint16 {
	const n = 16
	s := uint(k) & (n - 1)
	return x<<s | x>>(n-s)
}

// RotateLeft32 returns the value of x rotated left by (k mod 32) bits.
// To rotate x right by k bits, call RotateLeft32(x, -k).
//
// This function's execution time does not depend on the inputs.
func RotateLeft32(x uint32, k int) uint32 {
	const n = 32
	s := uint(k) & (n - 1)
	return x<<s | x>>(n-s)
}

// RotateLeft64 returns the value of x rotated left by (k mod 64) bits.
// To rotate x right by k bits, call RotateLeft64(x, -k).
//
// This function's execution time does not depend on the inputs.
func RotateLeft64(x uint64, k int) uint64 {
	const n = 64
	s := uint(k) & (n - 1)
	return x<<s | x>>(n-s)
}

// --- Len ---

// len8tab is vendored verbatim from Go 1.26 math/bits (src/math/bits/bits_tab.go).
// Upstream keeps it in a separate file bits_tab.go; it is inlined here only
// because Goo's multi-file package concatenation is not yet exercised. The
// table bytes are unchanged.
const len8tab = "" +
	"\x00\x01\x02\x02\x03\x03\x03\x03\x04\x04\x04\x04\x04\x04\x04\x04" +
	"\x05\x05\x05\x05\x05\x05\x05\x05\x05\x05\x05\x05\x05\x05\x05\x05" +
	"\x06\x06\x06\x06\x06\x06\x06\x06\x06\x06\x06\x06\x06\x06\x06\x06" +
	"\x06\x06\x06\x06\x06\x06\x06\x06\x06\x06\x06\x06\x06\x06\x06\x06" +
	"\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07" +
	"\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07" +
	"\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07" +
	"\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07" +
	"\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08" +
	"\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08" +
	"\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08" +
	"\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08" +
	"\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08" +
	"\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08" +
	"\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08" +
	"\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08\x08"

// Len8 returns the minimum number of bits required to represent x; the result is 0 for x == 0.
func Len8(x uint8) int {
	return int(len8tab[x])
}

// Len16 returns the minimum number of bits required to represent x; the result is 0 for x == 0.
func Len16(x uint16) (n int) {
	if x >= 1<<8 {
		x >>= 8
		n = 8
	}
	return n + int(len8tab[x])
}

// Len32 returns the minimum number of bits required to represent x; the result is 0 for x == 0.
func Len32(x uint32) (n int) {
	if x >= 1<<16 {
		x >>= 16
		n = 16
	}
	if x >= 1<<8 {
		x >>= 8
		n += 8
	}
	return n + int(len8tab[x])
}

// Len64 returns the minimum number of bits required to represent x; the result is 0 for x == 0.
func Len64(x uint64) (n int) {
	if x >= 1<<32 {
		x >>= 32
		n = 32
	}
	if x >= 1<<16 {
		x >>= 16
		n += 16
	}
	if x >= 1<<8 {
		x >>= 8
		n += 8
	}
	return n + int(len8tab[x])
}

// --- LeadingZeros ---

// LeadingZeros8 returns the number of leading zero bits in x; the result is 8 for x == 0.
func LeadingZeros8(x uint8) int { return 8 - Len8(x) }

// LeadingZeros16 returns the number of leading zero bits in x; the result is 16 for x == 0.
func LeadingZeros16(x uint16) int { return 16 - Len16(x) }

// LeadingZeros32 returns the number of leading zero bits in x; the result is 32 for x == 0.
func LeadingZeros32(x uint32) int { return 32 - Len32(x) }

// LeadingZeros64 returns the number of leading zero bits in x; the result is 64 for x == 0.
func LeadingZeros64(x uint64) int { return 64 - Len64(x) }

// --- TrailingZeros ---

// ntz8tab is vendored verbatim from Go 1.26 math/bits (src/math/bits/bits_tab.go),
// inlined here (see len8tab). ntz8tab[i] is the number of trailing zeros of i,
// with ntz8tab[0] == 8.
const ntz8tab = "" +
	"\x08\x00\x01\x00\x02\x00\x01\x00\x03\x00\x01\x00\x02\x00\x01\x00" +
	"\x04\x00\x01\x00\x02\x00\x01\x00\x03\x00\x01\x00\x02\x00\x01\x00" +
	"\x05\x00\x01\x00\x02\x00\x01\x00\x03\x00\x01\x00\x02\x00\x01\x00" +
	"\x04\x00\x01\x00\x02\x00\x01\x00\x03\x00\x01\x00\x02\x00\x01\x00" +
	"\x06\x00\x01\x00\x02\x00\x01\x00\x03\x00\x01\x00\x02\x00\x01\x00" +
	"\x04\x00\x01\x00\x02\x00\x01\x00\x03\x00\x01\x00\x02\x00\x01\x00" +
	"\x05\x00\x01\x00\x02\x00\x01\x00\x03\x00\x01\x00\x02\x00\x01\x00" +
	"\x04\x00\x01\x00\x02\x00\x01\x00\x03\x00\x01\x00\x02\x00\x01\x00" +
	"\x07\x00\x01\x00\x02\x00\x01\x00\x03\x00\x01\x00\x02\x00\x01\x00" +
	"\x04\x00\x01\x00\x02\x00\x01\x00\x03\x00\x01\x00\x02\x00\x01\x00" +
	"\x05\x00\x01\x00\x02\x00\x01\x00\x03\x00\x01\x00\x02\x00\x01\x00" +
	"\x04\x00\x01\x00\x02\x00\x01\x00\x03\x00\x01\x00\x02\x00\x01\x00" +
	"\x06\x00\x01\x00\x02\x00\x01\x00\x03\x00\x01\x00\x02\x00\x01\x00" +
	"\x04\x00\x01\x00\x02\x00\x01\x00\x03\x00\x01\x00\x02\x00\x01\x00" +
	"\x05\x00\x01\x00\x02\x00\x01\x00\x03\x00\x01\x00\x02\x00\x01\x00" +
	"\x04\x00\x01\x00\x02\x00\x01\x00\x03\x00\x01\x00\x02\x00\x01\x00"

// TrailingZeros8 returns the number of trailing zero bits in x; the result is 8 for x == 0.
func TrailingZeros8(x uint8) int {
	return int(ntz8tab[x])
}

const deBruijn32 = 0x077CB531

// deBruijn32tab is upstream's `var deBruijn32tab = [32]byte{...}` (bits.go),
// stored here as a const string because array literals are not yet supported.
// The bytes are unchanged; indexing a const string yields the same byte, so the
// function body below is verbatim. (Localized exception to the verbatim rule.)
const deBruijn32tab = "" +
	"\x00\x01\x1c\x02\x1d\x0e\x18\x03\x1e\x16\x14\x0f\x19\x11\x04\x08" +
	"\x1f\x1b\x0d\x17\x15\x13\x10\x07\x1a\x0c\x12\x06\x0b\x05\x0a\x09"

const deBruijn64 = 0x03f79d71b4ca8b09

// deBruijn64tab is upstream's `var deBruijn64tab = [64]byte{...}` (bits.go),
// stored here as a const string (see deBruijn32tab). Bytes unchanged.
const deBruijn64tab = "" +
	"\x00\x01\x38\x02\x39\x31\x1c\x03\x3d\x3a\x2a\x32\x26\x1d\x11\x04" +
	"\x3e\x2f\x3b\x24\x2d\x2b\x33\x16\x35\x27\x21\x1e\x18\x12\x0c\x05" +
	"\x3f\x37\x30\x1b\x3c\x29\x25\x10\x2e\x23\x2c\x15\x34\x20\x17\x0b" +
	"\x36\x1a\x28\x0f\x22\x14\x1f\x0a\x19\x0e\x13\x09\x0d\x08\x07\x06"

// TrailingZeros16 returns the number of trailing zero bits in x; the result is 16 for x == 0.
func TrailingZeros16(x uint16) int {
	if x == 0 {
		return 16
	}
	// see comment in TrailingZeros64
	return int(deBruijn32tab[uint32(x&-x)*deBruijn32>>27])
}

// TrailingZeros32 returns the number of trailing zero bits in x; the result is 32 for x == 0.
func TrailingZeros32(x uint32) int {
	if x == 0 {
		return 32
	}
	// see comment in TrailingZeros64
	return int(deBruijn32tab[(x&-x)*deBruijn32>>27])
}

// TrailingZeros64 returns the number of trailing zero bits in x; the result is 64 for x == 0.
func TrailingZeros64(x uint64) int {
	if x == 0 {
		return 64
	}
	// If popcount is fast, replace code below with return popcount(^x & (x - 1)).
	//
	// x & -x leaves only the right-most bit set in the word. Let k be the
	// index of that bit. Since only a single bit is set, the value is two
	// to the power of k. Multiplying by a power of two is equivalent to
	// left shifting, in this case by k bits. The de Bruijn (64 bit) constant
	// is such that all six bit, consecutive substrings are distinct.
	// Therefore, if we have a left shifted version of this constant we can
	// find by how many bits it was shifted by looking at which six bit
	// substring ended up at the top of the word.
	// (Knuth, volume 4, section 7.3.1)
	return int(deBruijn64tab[(x&-x)*deBruijn64>>58])
}

// --- OnesCount ---

// pop8tab is vendored verbatim from Go 1.26 math/bits (src/math/bits/bits_tab.go),
// inlined here (see len8tab). pop8tab[i] is the number of one bits in i.
const pop8tab = "" +
	"\x00\x01\x01\x02\x01\x02\x02\x03\x01\x02\x02\x03\x02\x03\x03\x04" +
	"\x01\x02\x02\x03\x02\x03\x03\x04\x02\x03\x03\x04\x03\x04\x04\x05" +
	"\x01\x02\x02\x03\x02\x03\x03\x04\x02\x03\x03\x04\x03\x04\x04\x05" +
	"\x02\x03\x03\x04\x03\x04\x04\x05\x03\x04\x04\x05\x04\x05\x05\x06" +
	"\x01\x02\x02\x03\x02\x03\x03\x04\x02\x03\x03\x04\x03\x04\x04\x05" +
	"\x02\x03\x03\x04\x03\x04\x04\x05\x03\x04\x04\x05\x04\x05\x05\x06" +
	"\x02\x03\x03\x04\x03\x04\x04\x05\x03\x04\x04\x05\x04\x05\x05\x06" +
	"\x03\x04\x04\x05\x04\x05\x05\x06\x04\x05\x05\x06\x05\x06\x06\x07" +
	"\x01\x02\x02\x03\x02\x03\x03\x04\x02\x03\x03\x04\x03\x04\x04\x05" +
	"\x02\x03\x03\x04\x03\x04\x04\x05\x03\x04\x04\x05\x04\x05\x05\x06" +
	"\x02\x03\x03\x04\x03\x04\x04\x05\x03\x04\x04\x05\x04\x05\x05\x06" +
	"\x03\x04\x04\x05\x04\x05\x05\x06\x04\x05\x05\x06\x05\x06\x06\x07" +
	"\x02\x03\x03\x04\x03\x04\x04\x05\x03\x04\x04\x05\x04\x05\x05\x06" +
	"\x03\x04\x04\x05\x04\x05\x05\x06\x04\x05\x05\x06\x05\x06\x06\x07" +
	"\x03\x04\x04\x05\x04\x05\x05\x06\x04\x05\x05\x06\x05\x06\x06\x07" +
	"\x04\x05\x05\x06\x05\x06\x06\x07\x05\x06\x06\x07\x06\x07\x07\x08"

// OnesCount8 returns the number of one bits ("population count") in x.
func OnesCount8(x uint8) int {
	return int(pop8tab[x])
}

// OnesCount16 returns the number of one bits ("population count") in x.
func OnesCount16(x uint16) int {
	return int(pop8tab[x>>8] + pop8tab[x&0xff])
}

// OnesCount32 returns the number of one bits ("population count") in x.
func OnesCount32(x uint32) int {
	return int(pop8tab[x>>24] + pop8tab[x>>16&0xff] + pop8tab[x>>8&0xff] + pop8tab[x&0xff])
}
