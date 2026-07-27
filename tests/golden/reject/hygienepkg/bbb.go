// Second file of the SAME package. Uses fmt but does NOT import it.
//
// Go scopes imports per FILE, not per package, so this is an error even though
// the sibling file above imports fmt. Real go1.26.1 on the equivalent program:
//   hygpkg/bbb.go:4:2: undefined: fmt
package hygienepkg

func B() {
	fmt.Println("b")
}
