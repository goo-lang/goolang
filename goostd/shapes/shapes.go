package shapes

// Fixture package for P4.2/B1 (qualified type names in type position).
// Point is the exported struct type used by pkg_type_probe (var/param/
// return/field/slice positions) and the reject fixtures (unknown package,
// unexported type). NewPoint is the constructor — B1 deliberately does NOT
// support qualified composite literals (`shapes.Point{...}`, rider B4), so
// construction goes through this function instead.
type Point struct {
	X int
	Y int
}

func NewPoint(x int, y int) Point {
	return Point{X: x, Y: y}
}

// Sum is a value-receiver method — the P4.3/B2 cross-package method-call and
// method-value probes' primary target (pkg_method_probe).
func (p Point) Sum() int {
	return p.X + p.Y
}

// Scale is a pointer-receiver method — exercises the auto-&-of-addressable-
// value call path across a package boundary, and confirms the mutation is
// visible through the original main-side variable (pkg_method_probe).
func (p *Point) Scale(f int) {
	p.X = p.X * f
	p.Y = p.Y * f
}

// secret is an UNEXPORTED (lowercase) method — pkg_method_unexported_reject
// must show that Go's per-identifier export rule applies to methods too:
// the combined mangled name "Point__secret" starts with an uppercase letter
// (from the exported receiver Point) even though the method itself is not
// exported, so a naive "does the mangled name look exported" check would
// wrongly let this leak cross-package.
func (p Point) secret() int {
	return -1
}

// Version is a VALUE export (an ordinary package-level var, not a type
// declaration) — used by the value-member-as-type reject fixture to prove
// `var v shapes.Version` is rejected cleanly instead of silently
// typechecking as Version's own type (int).
var Version int = 1
