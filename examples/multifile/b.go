// File 2 of the same package. Declares Bang(), which a.go calls, and Twice(),
// which the probe calls directly — so the fixture pins both directions:
// package-internal cross-file resolution and cross-package export from the
// second file of a package.
package multifile

func Bang() string {
	return "!"
}

func Twice(n int) int {
	return n * 2
}
