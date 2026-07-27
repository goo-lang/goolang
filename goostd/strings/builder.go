// strings.Builder — accumulate a string without the quadratic cost of
// repeated `s = s + piece`.
//
// A third file of the strings package (PR #226 made a package the union of its
// files, so this is ordinary). It is the first goostd TYPE with methods outside
// goostd/lanes, which is what motivated teaching the coverage gate to extract
// receiver methods — before that change every method here would have shipped
// with no coverage enforcement at all.
//
// Go's Builder additionally forbids copying after first use, enforced with a
// self-pointer check, and exposes Write/WriteByte/WriteRune/Grow/Cap. This cut
// implements the four methods that carry the common case. The copy check is NOT
// implemented, and that is a real divergence rather than an oversight: see the
// note on Reset.
package strings

// Builder builds a string with successive WriteString calls.
//
// The zero value is ready to use, as in Go — no constructor, no init call. That
// is why buf is a nil slice rather than a preallocated one: `var b Builder`
// must work, and append to a nil slice is well defined.
type Builder struct {
	buf []byte
}

// WriteString appends s to the builder.
//
// Go returns (int, error) here to satisfy io.StringWriter, and its error is
// always nil. This cut returns nothing, because io does not exist yet and a
// two-value return that is always (len(s), nil) would be noise every caller has
// to discard. Revisit when io lands — it is an ADDITIVE change at that point.
func (b *Builder) WriteString(s string) {
	for i := 0; i < len(s); i++ {
		b.buf = append(b.buf, s[i])
	}
}

// String returns the accumulated string.
//
// Go returns an unsafe zero-copy view of the buffer. This converts, which
// copies. The observable behaviour is identical; only the cost differs.
func (b *Builder) String() string {
	return string(b.buf)
}

// Len returns the number of accumulated bytes, not runes — the same as Go.
func (b *Builder) Len() int {
	return len(b.buf)
}

// Reset truncates the builder back to empty and keeps it usable.
//
// Go drops the buffer entirely (b.buf = nil) so the next write reallocates.
// This does the same rather than reslicing to [:0], which would retain the old
// backing array. Retaining it would be the faster choice, but v1 has no
// systematic reclamation (see CLAUDE.md's memory model), so a Builder reused in
// a loop would hold its high-water allocation for the life of the program.
func (b *Builder) Reset() {
	b.buf = nil
}
