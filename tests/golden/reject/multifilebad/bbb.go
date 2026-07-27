// Second file. The type error below sits at line 6, column 2 OF THIS FILE.
package multifilebad

// padding so the line number below cannot coincide with a line number in aaa.go
func Bad() {
	var s string = 42
	_ = s
}
