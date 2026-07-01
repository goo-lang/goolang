// Vendored from Go 1.26 math/bits (src/math/bits/bits.go), verbatim.
// This is a curated SUBSET — the first functions proven to compile and run
// under Goo's frontend. It grows as more of the upstream file is unblocked by
// frontend features; nothing here is hand-adapted (the vendoring rule is
// upstream-verbatim, never edit real Go source).
package bits

// RotateLeft64 returns the value of x rotated left by (k mod 64) bits.
// To rotate x right by k bits, call RotateLeft64(x, -k).
//
// This function's execution time does not depend on the inputs.
func RotateLeft64(x uint64, k int) uint64 {
	const n = 64
	s := uint(k) & (n - 1)
	return x<<s | x>>(n-s)
}
