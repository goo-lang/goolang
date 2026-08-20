// Fixture package for the cross-package call probes. NOT stdlib: it is not in
// check_stdlib_coverage.sh's GOOSTD_PKG_DIRS, so package_toolchain.sh does not
// ship it. doc-claims-probe requires every goostd package to be one or the
// other, which is what stops a new package from silently never shipping.
package mypkg
func Double(n int) int { return n + n }
