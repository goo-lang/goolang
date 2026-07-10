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

// Version is a VALUE export (an ordinary package-level var, not a type
// declaration) — used by the value-member-as-type reject fixture to prove
// `var v shapes.Version` is rejected cleanly instead of silently
// typechecking as Version's own type (int).
var Version int = 1
