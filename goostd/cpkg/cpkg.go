// Compiler-test fixture package (like kinds/shapes/mypkg/pkgcheck/fwdref):
// cross-package comptime params. NOT stdlib — check_stdlib_coverage.sh's
// goostd-func extraction only walks GOOSTD_PKG_DIRS (strings/strconv/utf8/
// bits/lanes, the real stdlib source dirs); cpkg is named alongside its
// siblings in that script's own exclusion comment ("test-only goostd
// packages ... are compiler-test fixtures, not stdlib, and are intentionally
// out of scope here") and is deliberately outside its scope.
package cpkg

func Fill(comptime n int, seed int) int {
	var buf [n]int
	i := 0
	for i < n {
		buf[i] = seed + i
		i = i + 1
	}
	return buf[0] + buf[n-1]
}

// Boxed is a package-local struct used as a comptime function's return type —
// the exact shape that regressed B1 (a comptime-param package function
// referencing its own package's struct type in its signature). The instance is
// emitted during main's monomorphization pass, after cpkg's own scope was torn
// down, so `Boxed` must still resolve there.
type Boxed struct {
	first int
	last  int
}

func MakeBoxed(comptime n int, seed int) Boxed {
	var buf [n]int
	i := 0
	for i < n {
		buf[i] = seed + i
		i = i + 1
	}
	return Boxed{first: buf[0], last: buf[n-1]}
}

// Base backs the R1 fixture (arc2 Task 4, arc2-repro-report.md R1): a
// comptime package function reading a package-level var. FillFromBase must
// read Base at each call's actual comptime value, not a memoized/constant-
// folded read shared across instances — see
// examples/pkg_global_comptime_probe.goo for the two-instance proof.
var Base = 100

func FillFromBase(comptime n int, s int) int {
	return Base + s + n
}

// Box and its constructor Make back the R2 fixture (arc2 Task 4,
// arc2-repro-report.md R2): a comptime package function whose OTHER
// parameter is a package-local struct type, same shape as MakeBoxed's return
// type above but on the PARAMETER side instead. Read(comptime n int, b Box)
// is the exact repro shape. Make exists only because a package-qualified
// composite literal (`cpkg.Box{N: v}`) doesn't parse today (a known,
// out-of-scope grammar gap — see examples/pkg_comptime_structparam_probe.goo)
// so callers outside this package build a Box via Make instead of a literal.
type Box struct {
	N int
}

func Make(n int) Box {
	var b Box
	b.N = n
	return b
}

func Read(comptime n int, b Box) int {
	return b.N + n
}
