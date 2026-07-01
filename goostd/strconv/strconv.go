// Vendored from Go 1.26 strconv (src/strconv/atob.go), verbatim curated subset.
// strconv also has shim functions (Atoi) that remain a per-symbol fallback.
package strconv

// FormatBool returns "true" or "false" according to the value of b.
func FormatBool(b bool) string {
	if b {
		return "true"
	}
	return "false"
}
