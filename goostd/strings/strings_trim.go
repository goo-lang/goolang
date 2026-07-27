// Second file of the vendored `strings` package: cutset trimming and
// case-folding. Split out of strings.go so goostd itself exercises the
// multi-file package path on EVERY verify-core run, rather than leaving that
// path pinned only by a dedicated fixture. Before the multi-file arc all 11
// goostd packages were single-file, which is precisely why the path had never
// been exercised — strings.go was 555 lines and lanes.go 1032, both already
// straining against a limit nobody could reach.
//
// This file carries its OWN import of unicode/utf8, because imports are scoped
// per file (Go's rule), and it references runeSelf, which is declared in
// strings.go — so a single verify-core run covers all three properties the arc
// added: N files in one package, per-file imports, and cross-file resolution.
package strings

import "unicode/utf8"

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
