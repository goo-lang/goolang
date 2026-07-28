// Package io provides the basic interfaces to I/O primitives.
//
// This cut is the WRITE side only: Writer, plus the small helpers that need
// nothing else. Reader, os.Stdin, os.Open and bufio are the next arc, and
// nothing here is shaped to exclude them.
//
// Writer is a vendored SOURCE package rather than a C shim on purpose. It
// declares no behaviour at all — only an interface — so there is nothing for
// a shim table to hold, and a source package is what lets `*os.File` and
// `bytes.Buffer` satisfy it through ordinary structural satisfaction.
package io

// Writer is the interface that wraps the basic Write method.
//
// Write writes len(p) bytes from p to the underlying data stream. It returns
// the number of bytes written and any error that stopped the write early.
// Write must return a non-nil error if it returns n < len(p).
//
// Write must not modify p, even temporarily.
type Writer interface {
	Write(p []byte) (int, error)
}

// WriteString writes the contents of s to w.
//
// Go declares this against an io.StringWriter fast path when the writer has
// one. There is no StringWriter in this cut, so it always goes through the
// []byte conversion — same observable result, one copy.
func WriteString(w Writer, s string) (int, error) {
	return w.Write([]byte(s))
}
