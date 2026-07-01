package pkgcheck

// Fixture for pkg-argcheck-probe: exercises cross-package call type-checking.
// Half's int64 param vs an i32 literal is the width-mismatch case that must be
// rejected at type-check (not reach the LLVM verifier); Double drives the
// arity and happy-path cases.
func Half(n int64) int64 { return n / 2 }
func Double(n int) int   { return n + n }
