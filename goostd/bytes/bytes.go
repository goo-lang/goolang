// Package bytes implements functions for the manipulation of byte slices.
//
// This cut is Buffer's WRITE side, which is what makes io.Writer testable: an
// interface with one implementation proves nothing, and a file is a writer you
// cannot look inside. A Buffer is one you can.
//
// Go's Buffer also reads (Read, ReadString, Next, ...) and carries an
// off/lastRead pair to track consumed bytes. None of that is here, and no
// field is reserved for it — the read side arrives with io.Reader, in the arc
// after this one.
package bytes

// A Buffer is a variable-sized buffer of bytes with Write methods.
//
// The zero value is an empty buffer ready to use, which is Go's contract and
// costs nothing here: a nil []byte appends correctly.
type Buffer struct {
	buf []byte
}

// Write appends p to the buffer, growing it as needed.
//
// The result is always (len(p), nil): the only failure a real writer reports
// is one the underlying stream raises, and a buffer has none. Go says the same
// — its Buffer.Write documents err as always nil, and panics with ErrTooLarge
// rather than returning an error when it cannot grow.
func (b *Buffer) Write(p []byte) (int, error) {
	b.buf = append(b.buf, p...)
	return len(p), nil
}

// WriteString appends s to the buffer.
//
// Go implements this separately to avoid the []byte conversion's copy. Goo's
// conversion copies too, so this is the same one-copy cost as Write and is
// kept for the API rather than for speed.
func (b *Buffer) WriteString(s string) (int, error) {
	return b.Write([]byte(s))
}

// String returns the buffer's contents as a string.
//
// A copy, so mutating the buffer afterwards does not change a string already
// returned — the same guarantee Goo's string([]byte) conversion gives
// everywhere else.
func (b *Buffer) String() string {
	return string(b.buf)
}

// Len returns the number of bytes in the buffer.
func (b *Buffer) Len() int {
	return len(b.buf)
}

// Reset truncates the buffer to empty, keeping the storage for reuse.
//
// Re-slicing to zero rather than assigning a fresh nil is what makes reuse
// cheap: the capacity survives, so a Buffer used in a loop stops allocating
// once it reaches its high-water mark.
func (b *Buffer) Reset() {
	b.buf = b.buf[:0]
}
