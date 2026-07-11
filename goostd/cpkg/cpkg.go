// Compiler-test fixture (like kinds/shapes): cross-package comptime params.
// NOT stdlib; deliberately outside check_stdlib_coverage.sh's scope.
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
