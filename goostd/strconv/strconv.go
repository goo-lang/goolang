// Vendored from Go 1.26 strconv (src/strconv/atob.go), verbatim curated subset.
// strconv also has shim functions (Atoi) that remain a per-symbol fallback.
package strconv

import (
	"bits"
	"utf8"
)

// FormatBool returns "true" or "false" according to the value of b.
func FormatBool(b bool) string {
	if b {
		return "true"
	}
	return "false"
}

// --- FormatInt (P4.10) ---
//
// Vendored from Go 1.21 strconv/itoa.go (src/strconv/itoa.go): the small-int
// fast path (small/nSmalls/smallsString) plus formatBits' digit loop. Bases
// 10 and 16 are the acceptance gate; formatBits is base-parametric so 2..36
// come free (untested beyond 10/16 here).
//
// DEVIATIONS from verbatim (documented per the goostd/bits deBruijn-comment
// convention):
//  1. Upstream's formatBits base==10 branch has a `host32bit`-guarded 32-bit
//     chunked-division fast path (`for u >= 1e9 {...}`) — a micro-
//     optimization for 32-bit hosts, unsafe/append-adjacent machinery out of
//     scope here. Dropped per the vendoring brief ("take the portable pure
//     paths"). The portable `us := uint(u); for us >= 100 {...}` loop that
//     follows it upstream is kept verbatim and produces byte-identical
//     output on every input regardless of host width — verified against
//     `go run` for base 10/16 across zero/positive/negative/MinInt64/
//     MaxInt64.
//  2. formatBits' signature is narrowed from upstream's
//     `(dst []byte, u uint64, base int, neg, append_ bool) (d []byte, s string)`
//     to `(u uint64, base int, neg bool) string`: the dst/append_ plumbing
//     only exists upstream to share code with AppendInt/FormatUint/
//     AppendUint, none of which this vendoring includes (out of scope per
//     the roadmap item) — dead parameters removed, the digit-computation
//     body is otherwise unchanged.
//  3. `var a [64+1]byte` (a fixed-size stack array, sliced at the end as
//     `a[i:]`) becomes `a := make([]byte, 65)`: Goo's v1 frontend does not
//     support slicing a fixed-size array (`Cannot slice type [24]uint8`,
//     confirmed by probe) — a heap slice of the same length is used
//     instead. Purely a storage-class change; the indexing/fill logic is
//     unchanged.

const digits = "0123456789abcdefghijklmnopqrstuvwxyz"

const nSmalls = 100

// smallsString is the formatting of 00..99 concatenated (verbatim).
const smallsString = "00010203040506070809" +
	"10111213141516171819" +
	"20212223242526272829" +
	"30313233343536373839" +
	"40414243444546474849" +
	"50515253545556575859" +
	"60616263646566676869" +
	"70717273747576777879" +
	"80818283848586878889" +
	"90919293949596979899"

// small returns the string for an i with 0 <= i < nSmalls (verbatim).
func small(i int) string {
	if i < 10 {
		return digits[i : i+1]
	}
	return smallsString[i*2 : i*2+2]
}

// isPowerOfTwo (verbatim).
func isPowerOfTwo(x int) bool {
	return x&(x-1) == 0
}

// formatBits computes the string representation of u in the given base.
// If neg is set, u is the two's-complement bit pattern of a negative int64
// (see FormatInt's uint64(i) cast at the call site) and is negated back to
// its magnitude before conversion — verbatim algorithm, narrowed signature
// per deviation 2 above.
func formatBits(u uint64, base int, neg bool) string {
	if base < 2 || base > len(digits) {
		panic("strconv: illegal FormatInt base")
	}
	// 2 <= base && base <= len(digits)

	a := make([]byte, 65) // +1 for sign of 64bit value in base 2 (deviation 3)
	i := len(a)

	if neg {
		u = -u
	}

	// convert bits
	// We use uint values where we can because those will
	// fit into a single register even on a 32bit machine.
	if base == 10 {
		// common case: use constants for / because
		// the compiler can optimize it into a multiply+shift
		//
		// (deviation 1: upstream's host32bit-guarded 32-bit chunk loop
		// dropped here — this portable loop is upstream's own fallback
		// and is kept verbatim.)

		// u guaranteed to fit into a uint
		us := uint(u)
		for us >= 100 {
			is := us % 100 * 2
			us /= 100
			i -= 2
			a[i+1] = smallsString[is+1]
			a[i+0] = smallsString[is+0]
		}

		// us < 100
		is := us * 2
		i--
		a[i] = smallsString[is+1]
		if us >= 10 {
			i--
			a[i] = smallsString[is]
		}

	} else if isPowerOfTwo(base) {
		// Use shifts and masks instead of / and %.
		shift := uint(bits.TrailingZeros(uint(base))) & 7
		b := uint64(base)
		m := uint(base) - 1 // == 1<<shift - 1
		for u >= b {
			i--
			a[i] = digits[uint(u)&m]
			u >>= shift
		}
		// u < base
		i--
		a[i] = digits[uint(u)]
	} else {
		// general case
		b := uint64(base)
		for u >= b {
			i--
			// Avoid using r = a%b in addition to q = a/b
			// since 64bit division and modulo operations
			// are calculated by runtime functions on 32bit machines.
			q := u / b
			a[i] = digits[uint(u-q*b)]
			u = q
		}
		// u < base
		i--
		a[i] = digits[uint(u)]
	}

	// add sign, if any
	if neg {
		i--
		a[i] = '-'
	}

	return string(a[i:])
}

// FormatInt returns the string representation of i in the given base, for
// 2 <= base <= 36. The result uses the lower-case letters 'a' to 'z' for
// digit values >= 10. Verbatim algorithm (see deviations above for the
// storage-class and signature narrowing).
func FormatInt(i int64, base int) string {
	if 0 <= i && i < nSmalls && base == 10 {
		return small(int(i))
	}
	return formatBits(uint64(i), base, i < 0)
}

// --- ParseInt (P4.10) ---
//
// Vendored from Go 1.21 strconv/atoi.go (src/strconv/atoi.go)'s ParseUint
// decimal path (cutoff/overflow logic) + ParseInt's sign handling, ADAPTED
// per the roadmap: upstream is `ParseInt(s string, base int, bitSize int)
// (int64, error)`; this is the decimal-only (base 10), fixed-bitSize-64
// subset returning `!int64`, matching the FIRST goostd source function to
// return an error union (see the task's pre-verified-facts: `!T` returns
// from goostd source packages, and `error(...)` on the failure path, both
// confirmed working end-to-end before this file was written).
//
// Error messages mirror upstream's *NumError.Error() format exactly
// ("strconv.ParseInt: parsing %q: invalid syntax" / "...: value out of
// range", %q via this file's own Quote) — verified byte-for-byte against
// `go run` (see examples/strconv_vendored_probe.goo's header for the
// verification matrix), for every case except the two documented Quote
// deviations (which never trigger here since ParseInt's input is always
// re-quoted plain ASCII digits/sign).
func ParseInt(s string) !int64 {
	if s == "" {
		return error("strconv.ParseInt: parsing " + Quote(s) + ": invalid syntax")
	}

	// Pick off leading sign.
	s0 := s
	neg := false
	if s[0] == '+' {
		s = s[1:]
	} else if s[0] == '-' {
		neg = true
		s = s[1:]
	}

	if len(s) == 0 {
		// Sign alone ("+" or "-"): upstream's ParseUint("") also rejects
		// as ErrSyntax; keeping s0 (the pre-sign-strip original) in the
		// message matches upstream's NumError.Num convention.
		return error("strconv.ParseInt: parsing " + Quote(s0) + ": invalid syntax")
	}

	// Cutoff is the smallest number such that cutoff*10 > maxUint64
	// (upstream: `maxUint64/10 + 1`, a compile-time constant for base 10).
	const cutoff uint64 = 1844674407370955162
	// cutoff for the signed range at bitSize=64 (upstream: `uint64(1) << 63`).
	const maxInt64Cutoff uint64 = 9223372036854775808

	var n uint64
	for _, c := range []byte(s) {
		if c < '0' || c > '9' {
			return error("strconv.ParseInt: parsing " + Quote(s0) + ": invalid syntax")
		}
		d := c - '0'

		if n >= cutoff {
			// n*10 overflows
			return error("strconv.ParseInt: parsing " + Quote(s0) + ": value out of range")
		}
		n *= 10

		n1 := n + uint64(d)
		if n1 < n {
			// n+d overflows
			return error("strconv.ParseInt: parsing " + Quote(s0) + ": value out of range")
		}
		n = n1
	}

	if !neg && n >= maxInt64Cutoff {
		return error("strconv.ParseInt: parsing " + Quote(s0) + ": value out of range")
	}
	if neg && n > maxInt64Cutoff {
		return error("strconv.ParseInt: parsing " + Quote(s0) + ": value out of range")
	}
	result := int64(n)
	if neg {
		result = -result
	}
	return result
}

// --- Quote (P4.10) ---
//
// Vendored from Go 1.26 strconv/quote.go's quoteWith/appendQuotedWith/
// appendEscapedRune escape-loop STRUCTURE (the byte-at-a-time
// DecodeRuneInString scan, the invalid-UTF-8 \xNN short-circuit, the
// backslash-escape table, the \xNN / \uNNNN / \UNNNNNNNN tiers) with a
// SIMPLIFIED printability rule in place of upstream's unicode.IsPrint
// tables (not available — no unicode package in goostd):
//   - ASCII printable (0x20..0x7E), minus '"' and '\\' (always escaped),
//     is kept literal.
//   - The seven standard short escapes (\a \b \f \n \r \t \v) plus \" \\.
//   - Any other byte below utf8.RuneSelf (0x80) that isn't ASCII-printable
//     (control chars, DEL) escapes as \xNN.
//   - Any valid non-ASCII rune (r >= utf8.RuneSelf) escapes as \uNNNN
//     (r < 0x10000) or \UNNNNNNNN (r >= 0x10000) — DEVIATION: upstream
//     keeps a non-ASCII rune literal when unicode.IsPrint(r) is true. For
//     the single CJK character U+4E2D ('middle'): real Go's
//     strconv.Quote returns the 3-byte UTF-8 rune wrapped in quotes,
//     unchanged; this vendoring has no printability table for non-ASCII
//     runes, so it always escapes the code point instead, returning the
//     8-byte ASCII string "\u4e2d" (quote, backslash, u, 4, e, 2, d,
//     quote) — conservative and lossless (both round-trip to the same
//     code point), just never literal. Verified against `go run`:
//     identical output to upstream on every case except this one, which
//     is called out explicitly in the probe.
//
// A second deviation is structural, not behavioral: upstream's
// appendEscapedRune uses a TAGGED switch (`switch r { case '\a': ... }`).
// FIXED as of the correctness-burndown arc 2 (task 2): a tagged switch on a
// rune/int32 tag against char-literal cases now compiles and runs correctly
// (type_check_switch_stmt unifies an untyped-constant case expression's
// width with the tag's, representability-gated — see
// examples/switch_rune_char_probe.goo). This function is NOT rewritten back
// to a tagged switch here, though — that's a separate vendoring-fidelity
// pass (matching upstream's exact source shape token-for-token), not a
// correctness fix, so it's left as the tagless workaround below (still
// definitionally identical control flow, still confirmed compiling and
// matching `go run` byte-for-byte) until that pass happens.
//
// A third deviation: `utf8.RuneSelf` / `utf8.RuneError` (package-level
// CONST selectors on an imported goostd source package) do not resolve —
// confirmed compiler gap, see this file's end-of-file note. runeSelf/
// runeErrorRune below are local consts holding the identical values.

const lowerhex = "0123456789abcdef"

// runeSelf/runeErrorRune mirror utf8.RuneSelf (0x80) and utf8.RuneError
// (U+FFFD) — see the cross-package-const deviation note above.
const runeSelf = 0x80
const runeErrorRune = 0xFFFD

// isASCIIPrintable is the simplified printability rule (see Quote's doc
// comment) standing in for upstream's unicode.IsPrint for the ASCII range.
func isASCIIPrintable(r rune) bool {
	return r >= ' ' && r <= '~'
}

// appendEscapedRune mirrors upstream's function of the same name (ASCIIonly
// and graphicOnly are always false in this vendoring — Quote is the only
// caller, matching upstream's Quote = quoteWith(s, '"', false, false)).
func appendEscapedRune(buf []byte, r rune) []byte {
	if r == '"' || r == '\\' { // always backslashed
		buf = append(buf, '\\')
		buf = append(buf, byte(r))
		return buf
	}
	// Tagless switch — see the switch-on-rune codegen deviation above.
	switch {
	case r == '\a':
		buf = append(buf, `\a`...)
	case r == '\b':
		buf = append(buf, `\b`...)
	case r == '\f':
		buf = append(buf, `\f`...)
	case r == '\n':
		buf = append(buf, `\n`...)
	case r == '\r':
		buf = append(buf, `\r`...)
	case r == '\t':
		buf = append(buf, `\t`...)
	case r == '\v':
		buf = append(buf, `\v`...)
	case isASCIIPrintable(r):
		buf = append(buf, byte(r))
	case r < runeSelf:
		buf = append(buf, `\x`...)
		buf = append(buf, lowerhex[byte(r)>>4])
		buf = append(buf, lowerhex[byte(r)&0xF])
	case r < 0x10000:
		buf = append(buf, `\u`...)
		for sh := 12; sh >= 0; sh -= 4 {
			buf = append(buf, lowerhex[uint32(r)>>uint(sh)&0xF])
		}
	default:
		buf = append(buf, `\U`...)
		for sh := 28; sh >= 0; sh -= 4 {
			buf = append(buf, lowerhex[uint32(r)>>uint(sh)&0xF])
		}
	}
	return buf
}

// Quote returns a double-quoted Go string literal representing s, using Go
// escape sequences for control characters and non-printable/non-ASCII
// characters — see the deviation notes above for exactly where this departs
// from unicode.IsPrint-based upstream behaviour.
func Quote(s string) string {
	buf := make([]byte, 0, 3*len(s)/2)
	buf = append(buf, '"')
	for r, width := rune(0), 0; len(s) > 0; s = s[width:] {
		r, width = utf8.DecodeRuneInString(s)
		if width == 1 && r == runeErrorRune {
			buf = append(buf, `\x`...)
			buf = append(buf, lowerhex[s[0]>>4])
			buf = append(buf, lowerhex[s[0]&0xF])
			continue
		}
		buf = appendEscapedRune(buf, r)
	}
	buf = append(buf, '"')
	return string(buf)
}

// --- Compiler gaps found while vendoring (P4.10) ---
//
// 1. FIXED (correctness-burndown arc 2, task 2): switch-on-rune with
//    char-literal cases used to miscompile. A TAGGED switch (`switch r {
//    case '\n': ... }`) where the tag is `rune` (int32) and case values are
//    char literals used to fail LLVM module verification (`Both operands to
//    ICmp instruction are not of the same type! icmp eq i32 %r, i64 10` —
//    the case constant emitted as i64 while the tag stayed i32). Fixed in
//    type_check_switch_stmt (src/types/type_checker.c): an untyped-constant
//    case expression now unifies with the tag's type, representability-
//    gated (same rule as `return`'s int_const_fits_expected). Pinned by
//    examples/switch_rune_char_probe.goo. This function's tagless workaround
//    (`switch { case r == '\n': ... }`, semantically identical, still
//    compiles/runs correctly) is kept as-is — reverting it to a tagged
//    switch is a vendoring-fidelity pass, not a correctness fix; see the
//    Quote doc comment above.
//
// 2. Cross-package CONST/value-member selector access into a goostd SOURCE
//    package does not resolve: `utf8.RuneSelf` (a value, not a call) fails
//    "Undefined identifier 'utf8'" even at the simplest use
//    (`x := utf8.RuneSelf`), while `utf8.DecodeRuneInString(s)` (a function
//    CALL) resolves and runs correctly. The failure is in resolving the
//    base identifier `utf8` itself when the selector is not in call
//    position — cross-package FUNCTION calls go through a different,
//    working path (type_check_selector_expr's package-exports lookup,
//    confirmed working) than whatever handles the plain-identifier
//    resolution of `utf8.CONST` as a value expression. Confirmed with both
//    `utf8` (this task's dependency) and directly at the top level of
//    `main` (not specific to being inside a helper function or a
//    goostd-to-goostd import — the same failure reproduces for
//    `x := utf8.RuneSelf` in a plain main.goo). Workaround used in this
//    file: local consts (runeSelf, runeErrorRune) holding the identical
//    values, documented above. NOT fixed here (src/ off limits); flagged
//    for a follow-up type-checker fix — this blocks ANY future goostd
//    vendoring that needs a cross-package exported constant, not just
//    Quote's two here.
