// Vendored from Go 1.26 strings. HasPrefix/HasSuffix delegate to
// internal/stringslite upstream; their verbatim bodies (from
// src/internal/stringslite/strings.go) are used directly here since Goo has no
// internal-package layer. A curated SUBSET — grows as the frontend unblocks
// more; nothing hand-adapted beyond collapsing the one-line delegation.
//
// P4.9 additions (Index, Repeat, ReplaceAll, Fields, Count, TrimLeft,
// TrimRight, EqualFold) import unicode/utf8 (goostd/utf8) for rune decoding
// — this is the first goostd->goostd import. See each function's own
// comment for its upstream source and any documented deviation.
package strings

import "unicode/utf8"

// HasPrefix reports whether the string s begins with prefix.
func HasPrefix(s, prefix string) bool {
	return len(s) >= len(prefix) && s[:len(prefix)] == prefix
}

// HasSuffix reports whether the string s ends with suffix.
func HasSuffix(s, suffix string) bool {
	return len(s) >= len(suffix) && s[len(s)-len(suffix):] == suffix
}

// TrimPrefix returns s without the provided leading prefix string.
// If s doesn't start with prefix, s is returned unchanged.
func TrimPrefix(s, prefix string) string {
	if HasPrefix(s, prefix) {
		return s[len(prefix):]
	}
	return s
}

// TrimSuffix returns s without the provided trailing suffix string.
// If s doesn't end with suffix, s is returned unchanged.
func TrimSuffix(s, suffix string) string {
	if HasSuffix(s, suffix) {
		return s[:len(s)-len(suffix)]
	}
	return s
}

// --- P4.9 vendoring (Index, Repeat, ReplaceAll, Fields, Count, TrimLeft,
// TrimRight, EqualFold) below. Source: Go 1.9 src/strings/strings.go +
// strings_generic.go (raw.githubusercontent.com/golang/go/go1.9/src/strings/),
// the last release before internal/bytealg existed — the "pre-bytealg
// shape" upstream itself used for architectures without an assembly Index.
// Current (Go 1.26) upstream delegates Index/Count to internal/bytealg and
// internal/stringslite, neither of which exists in Goo; the Go 1.9 shape is
// the same algorithm (Rabin-Karp fallback) without that dependency. Each
// function below is upstream-verbatim unless marked DEVIATION.

// indexByte returns the index of the first instance of c in s, or -1 if c
// is not present in s.
//
// DEVIATION: upstream's IndexByte is architecture-specific assembly
// (strings_decl.go declares it as `// ../runtime/asm_$GOARCH.s` with no
// portable Go body to vendor). This is a straightforward byte-scan loop
// with identical semantics, kept unexported since only Index below (P4.9's
// requested surface) needs it.
func indexByte(s string, c byte) int {
	for i := 0; i < len(s); i++ {
		if s[i] == c {
			return i
		}
	}
	return -1
}

// primeRK is the prime base used in the Rabin-Karp algorithm below.
const primeRK = 16777619

// hashStr returns the hash and the appropriate multiplicative factor for
// use in the Rabin-Karp algorithm.
//
// DEVIATION: `var pow, sq uint32 = 1, primeRK` (multi-variable declaration
// with an explicit type AND an initializer list) does not parse in Goo;
// split into two single-variable declarations, same values.
func hashStr(sep string) (uint32, uint32) {
	hash := uint32(0)
	for i := 0; i < len(sep); i++ {
		hash = hash*primeRK + uint32(sep[i])
	}
	var pow uint32 = 1
	var sq uint32 = primeRK
	for i := len(sep); i > 0; i >>= 1 {
		if i&1 != 0 {
			pow *= sq
		}
		sq *= sq
	}
	return hash, pow
}

// Index returns the index of the first instance of substr in s, or -1 if
// substr is not present in s.
//
// DEVIATION: upstream (Go 1.26) delegates to internal/bytealg (SIMD asm on
// supported architectures) via internal/stringslite; neither package exists
// in Goo. This is upstream's own portable fallback for architectures
// without an assembly Index (Go 1.9 strings_generic.go, `+build
// !amd64,!s390x`, before bytealg existed) vendored verbatim: trivial-length
// special cases, then Rabin-Karp search. The inner scan loop is
// restructured from a 3-clause `for i := n; i < len(s); { ... }` (empty
// post-clause) to an equivalent `for i < len(s) { ... i++ }` — Goo's parser
// does not accept a 3-clause for with an empty third clause; the increment
// still executes exactly once per iteration in the same place.
func Index(s, substr string) int {
	n := len(substr)
	switch {
	case n == 0:
		return 0
	case n == 1:
		return indexByte(s, substr[0])
	case n == len(s):
		if substr == s {
			return 0
		}
		return -1
	case n > len(s):
		return -1
	}
	// Rabin-Karp search
	hashss, pow := hashStr(substr)
	var h uint32
	for i := 0; i < n; i++ {
		h = h*primeRK + uint32(s[i])
	}
	if h == hashss && s[:n] == substr {
		return 0
	}
	i := n
	for i < len(s) {
		h *= primeRK
		h += uint32(s[i])
		h -= pow * uint32(s[i-n])
		i++
		if h == hashss && s[i-n:i] == substr {
			return i - n
		}
	}
	return -1
}

// Count counts the number of non-overlapping instances of substr in s.
// If substr is an empty string, Count returns 1 + the number of Unicode
// code points in s.
//
// DEVIATION: upstream splits this across two files by GOARCH build tag —
// strings.go's countGeneric (used by architectures without an assembly
// Index) plumbed through strings_generic.go's one-line Count wrapper. Goo
// has a single Index regardless of target, so the wrapper and its body are
// folded into one function here; the logic is byte-for-byte unchanged
// (including overlap semantics: matches are found left-to-right and
// non-overlapping, exactly like real Go).
func Count(s, substr string) int {
	if len(substr) == 0 {
		return utf8.RuneCountInString(s) + 1
	}
	n := 0
	for {
		i := Index(s, substr)
		if i == -1 {
			return n
		}
		n++
		s = s[i+len(substr):]
	}
}

// Repeat returns a new string consisting of count copies of the string s.
//
// It panics if count is negative or if the result of (len(s) * count)
// overflows.
func Repeat(s string, count int) string {
	if count < 0 {
		panic("strings: negative Repeat count")
	} else if count > 0 && len(s)*count/count != len(s) {
		panic("strings: Repeat count causes overflow")
	}

	b := make([]byte, len(s)*count)
	bp := copy(b, s)
	for bp < len(b) {
		copy(b[bp:], b[:bp])
		bp *= 2
	}
	return string(b)
}

// Replace returns a copy of the string s with the first n non-overlapping
// instances of old replaced by new. If old is empty, it matches at the
// beginning of the string and after each UTF-8 sequence, yielding up to
// k+1 replacements for a k-rune string. If n < 0, there is no limit on the
// number of replacements.
func Replace(s, old, new string, n int) string {
	if old == new || n == 0 {
		return s // avoid allocation
	}

	// Compute number of replacements.
	if m := Count(s, old); m == 0 {
		return s // avoid allocation
	} else if n < 0 || m < n {
		n = m
	}

	// Apply replacements to buffer.
	t := make([]byte, len(s)+n*(len(new)-len(old)))
	w := 0
	start := 0
	for i := 0; i < n; i++ {
		j := start
		if len(old) == 0 {
			if i > 0 {
				_, wid := utf8.DecodeRuneInString(s[start:])
				j += wid
			}
		} else {
			j += Index(s[start:], old)
		}
		w += copy(t[w:], s[start:j])
		w += copy(t[w:], new)
		start = j + len(old)
	}
	w += copy(t[w:], s[start:])
	return string(t[0:w])
}

// ReplaceAll returns a copy of the string s with all non-overlapping
// instances of old replaced by new.
func ReplaceAll(s, old, new string) string {
	return Replace(s, old, new, -1)
}

// runeSelf mirrors utf8.RuneSelf (0x80 — bytes below this value are a
// complete single-byte UTF-8 encoding of themselves).
//
// COMPILER GAP, not a vendoring deviation: Goo's codegen does not resolve
// package-qualified CONSTANT selectors from a source package. Probed in
// isolation — `x := utf8.RuneSelf` alone fails ("Undefined identifier
// 'utf8'" / "Failed to generate base expression for selector"), while the
// equivalent function CALL `utf8.DecodeRuneInString(...)` resolves fine,
// so the gap is specific to const selectors, not cross-package imports in
// general (goostd/utf8's exported functions all resolve as expected). This
// local copy is byte-identical to utf8.RuneSelf (fixed by the UTF-8 spec,
// never changes) — flagged here for a follow-up compiler fix
// (package-qualified const selector resolution) rather than worked around
// in src/, per this task's scope (goostd-only, no compiler changes).
const runeSelf = 0x80

// asciiSpace is a lookup table for the ASCII whitespace bytes, used by
// Fields' fast path.
var asciiSpace = [256]uint8{'\t': 1, '\n': 1, '\v': 1, '\f': 1, '\r': 1, ' ': 1}

// isSpaceRune reports whether r is one of the whitespace runes recognized
// by Fields' non-ASCII slow path.
//
// DEVIATION: upstream uses unicode.IsSpace, which does not exist in Goo (no
// `unicode` package — see the goostd/utf8 header). This covers exactly the
// Latin-1 whitespace set unicode.IsSpace recognizes: the six ASCII
// whitespace bytes ('\t' '\n' '\v' '\f' '\r' ' ') plus NEL U+0085 and NBSP
// U+00A0. It does NOT cover the remaining Unicode White_Space code points
// (U+2000-U+200A, U+2028, U+2029, U+202F, U+205F, U+3000, U+1680) that real
// unicode.IsSpace also treats as space — a documented, narrower-than-Go gap;
// full Unicode White_Space coverage is a post-v1 unicode-tables item. Also
// restructured from a `switch r { case ' ', '\t', ...: }` case-list (Goo's
// parser does not accept comma-separated values in one case) to chained
// `||` comparisons.
func isSpaceRune(r rune) bool {
	return r == ' ' || r == '\t' || r == '\n' || r == '\v' || r == '\f' || r == '\r' || r == 0x85 || r == 0xA0
}

// Fields splits the string s around each instance of one or more
// consecutive white space characters, returning a slice of substrings of s
// or an empty slice if s contains only white space.
//
// COMPILER GAP WORKAROUND: the counting loop's per-byte variable is named
// `cb` (not upstream's `r`) so it can never share a name with the decoded-
// rune variables (`dr`/`dw`, below) used later in this same function. Goo's
// checker was found (probed in isolation, see runeSelf's comment above for
// the mechanism) to sometimes resolve a `name, name2 := twoRet()` in a
// later sibling block to the TYPE of an earlier, already-closed sibling
// block's identically-named variable instead of the fresh call's actual
// return type — reusing `r` for both a byte (this loop) and a rune
// (below) reproduced exactly that failure ("cannot use uint8 as int32").
// Distinct names sidestep it; no behavior change.
func Fields(s string) []string {
	// First count the fields.
	// This is an exact count if s is ASCII, otherwise it is an approximation.
	n := 0
	wasSpace := 1
	// setBits is used to track which bits are set in the bytes of s.
	setBits := uint8(0)
	for i := 0; i < len(s); i++ {
		cb := s[i]
		setBits |= cb
		isSpace := int(asciiSpace[cb])
		n += wasSpace & ^isSpace
		wasSpace = isSpace
	}

	if setBits < runeSelf { // ASCII fast path
		a := make([]string, n)
		na := 0
		fieldStart := 0
		i := 0
		// Skip spaces in the front of the input.
		//
		// COMPILER GAP WORKAROUND: upstream's single-condition
		// `for i < len(s) && asciiSpace[s[i]] != 0 { i++ }` compiles here to
		// invalid LLVM IR ("ZExt only operates on integer") — indexing an
		// array directly with a nested string-index expression
		// (`asciiSpace[s[i]]`) emits the string-char GEP's raw pointer into
		// the index-widening path instead of loading it first (probed in
		// isolation: `tbl[s[i]]` alone reproduces it; `c := s[i]; tbl[c]`
		// does not). Binding `s[i]` to a plain local (`c`/`c2` below) before
		// indexing forces the load and sidesteps it; restructured from a
		// combined loop condition to an explicit body check for the same
		// reason (the condition position has the identical nested-index
		// shape). Behavior is unchanged — same scan, same stopping rule.
		for i < len(s) {
			c := s[i]
			if asciiSpace[c] == 0 {
				break
			}
			i++
		}
		fieldStart = i
		for i < len(s) {
			c := s[i]
			if asciiSpace[c] == 0 {
				i++
				continue
			}
			a[na] = s[fieldStart:i]
			na++
			i++
			// Skip spaces in between fields.
			for i < len(s) {
				c2 := s[i]
				if asciiSpace[c2] == 0 {
					break
				}
				i++
			}
			fieldStart = i
		}
		if fieldStart < len(s) { // Last field might end at EOF.
			a[na] = s[fieldStart:]
		}
		return a
	}

	// Some runes in the input string are not ASCII. Same general approach
	// as the ASCII path but decodes via utf8.DecodeRuneInString and checks
	// isSpaceRune (DEVIATION note above) when a non-ASCII rune is hit.
	a := make([]string, 0, n)
	fieldStart := 0
	i := 0
	// Skip spaces in the front of the input.
	for i < len(s) {
		if c := s[i]; c < runeSelf {
			if asciiSpace[c] == 0 {
				break
			}
			i++
		} else {
			dr, dw := utf8.DecodeRuneInString(s[i:])
			if !isSpaceRune(dr) {
				break
			}
			i += dw
		}
	}
	fieldStart = i
	for i < len(s) {
		if c := s[i]; c < runeSelf {
			if asciiSpace[c] == 0 {
				i++
				continue
			}
			a = append(a, s[fieldStart:i])
			i++
		} else {
			dr, dw := utf8.DecodeRuneInString(s[i:])
			if !isSpaceRune(dr) {
				i += dw
				continue
			}
			a = append(a, s[fieldStart:i])
			i += dw
		}
		// Skip spaces in between fields.
		for i < len(s) {
			if c := s[i]; c < runeSelf {
				if asciiSpace[c] == 0 {
					break
				}
				i++
			} else {
				dr, dw := utf8.DecodeRuneInString(s[i:])
				if !isSpaceRune(dr) {
					break
				}
				i += dw
			}
		}
		fieldStart = i
	}
	if fieldStart < len(s) { // Last field might end at EOF.
		a = append(a, s[fieldStart:])
	}
	return a
}

// runeInCutset reports whether r appears, as a decoded rune, anywhere in
// cutset. Shared by TrimLeft and TrimRight below.
//
// DEVIATION: upstream builds a cutset-classifier closure (makeCutsetFunc)
// that special-cases a single-byte cutset, an all-ASCII cutset (via an
// asciiSet bitmap for O(1) membership), and a general Unicode cutset (via
// IndexRune, which decodes cutset with a `for i, r := range cutset` —
// ranging over a string decodes UTF-8 runes in real Go). Goo's `for range`
// over a string iterates raw BYTES rather than decoded runes (see
// examples/range_string_probe.goo), so that composition does not carry
// over unchanged. This collapses it to one explicit linear scan of cutset
// using utf8.DecodeRuneInString stepping, used uniformly by both TrimLeft
// and TrimRight — cutsets are small in every realistic call (a handful of
// runes), so the O(len(cutset)) scan per boundary rune costs nothing the
// upstream asciiSet fast path exists to avoid at scale.
func runeInCutset(r rune, cutset string) bool {
	i := 0
	for i < len(cutset) {
		cr, w := utf8.DecodeRuneInString(cutset[i:])
		if cr == r {
			return true
		}
		i += w
	}
	return false
}

// TrimLeft returns a slice of the string s with all leading Unicode code
// points contained in cutset removed.
func TrimLeft(s string, cutset string) string {
	if s == "" || cutset == "" {
		return s
	}
	i := 0
	for i < len(s) {
		r, w := utf8.DecodeRuneInString(s[i:])
		if !runeInCutset(r, cutset) {
			break
		}
		i += w
	}
	return s[i:]
}

// TrimRight returns a slice of the string s, with all trailing Unicode code
// points contained in cutset removed.
//
// DEVIATION: upstream scans backward from the end of s using
// utf8.DecodeLastRuneInString; goostd/utf8 vendors only the forward decoder
// (DecodeRune/DecodeRuneInString — see goostd/utf8/utf8.go). This does a
// single forward pass recording each rune's start byte offset, then walks
// that offset list back-to-front — identical output, no backward decoder
// needed.
func TrimRight(s string, cutset string) string {
	if s == "" || cutset == "" {
		return s
	}
	starts := make([]int, 0, len(s))
	i := 0
	for i < len(s) {
		starts = append(starts, i)
		_, w := utf8.DecodeRuneInString(s[i:])
		i += w
	}
	end := len(s)
	for k := len(starts) - 1; k >= 0; k-- {
		start := starts[k]
		r, _ := utf8.DecodeRuneInString(s[start:])
		if !runeInCutset(r, cutset) {
			break
		}
		end = start
	}
	return s[:end]
}

// EqualFold reports whether s and t, interpreted as UTF-8 strings, are
// equal under Unicode case-folding.
//
// DEVIATION: upstream's general (non-ASCII) case walks unicode.SimpleFold's
// orbit for full Unicode case folding (e.g. the Kelvin sign U+212A folds
// equal to 'k'/'K'). The `unicode` package does not exist in Goo (see
// goostd/utf8 header). The ASCII fast path below (A-Z folds to a-z, either
// direction) is upstream-verbatim and complete; runes that reach the
// general case — already unequal, and not an ASCII upper/lower pair — are
// treated as not equal rather than walked through a fold orbit. This
// under-approximates upstream for non-ASCII case pairs; documented and
// exercised by the probe (ASCII folding asserted correct both directions,
// the Kelvin-sign/'k' pair asserted NOT folded — the known gap). Also split
// the upstream `sr, s = rune(s[0]), s[1:]` tuple assignment into two plain
// statements; Goo's multi-value plain assignment works (probed), this is
// purely a style choice to keep each branch's two effects visually distinct.
func EqualFold(s, t string) bool {
	for s != "" && t != "" {
		var sr, tr rune
		if s[0] < runeSelf {
			sr = rune(s[0])
			s = s[1:]
		} else {
			r, size := utf8.DecodeRuneInString(s)
			sr = r
			s = s[size:]
		}
		if t[0] < runeSelf {
			tr = rune(t[0])
			t = t[1:]
		} else {
			r, size := utf8.DecodeRuneInString(t)
			tr = r
			t = t[size:]
		}

		// Easy case.
		if tr == sr {
			continue
		}

		// Make sr < tr to simplify what follows (mirrors upstream).
		if tr < sr {
			sr, tr = tr, sr
		}
		// Fast check for ASCII.
		if tr < runeSelf && 'A' <= sr && sr <= 'Z' {
			// ASCII, and sr is upper case. tr must be lower case.
			if tr == sr+'a'-'A' {
				continue
			}
			return false
		}

		// General case: no unicode.SimpleFold available (DEVIATION note
		// above) — anything reaching here already failed both the exact
		// match and the ASCII fold check, so it is not a fold match.
		return false
	}

	// One string is empty. Are both?
	return s == t
}
