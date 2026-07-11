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
