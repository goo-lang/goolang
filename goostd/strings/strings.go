// Vendored from Go 1.26 strings. HasPrefix/HasSuffix delegate to
// internal/stringslite upstream; their verbatim bodies (from
// src/internal/stringslite/strings.go) are used directly here since Goo has no
// internal-package layer. A curated SUBSET — grows as the frontend unblocks
// more; nothing hand-adapted beyond collapsing the one-line delegation.
package strings

// HasPrefix reports whether the string s begins with prefix.
func HasPrefix(s, prefix string) bool {
	return len(s) >= len(prefix) && s[:len(prefix)] == prefix
}

// HasSuffix reports whether the string s ends with suffix.
func HasSuffix(s, suffix string) bool {
	return len(s) >= len(suffix) && s[len(s)-len(suffix):] == suffix
}
