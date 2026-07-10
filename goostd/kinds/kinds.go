package kinds

// Fixture package for P4.3 review fix (MAJOR): exports a QUALIFIED interface
// (Shaper) together with an implementing struct (Rect) so main can hold a
// `kinds.Shaper` variable, assign a package-owned concrete into it, and
// dispatch through the vtable — the interface-satisfaction method lookup and
// the codegen thunk builder must both resolve Rect's Area through kinds'
// exports (prefix-first), not main's scope.
type Shaper interface {
	Area() int
}

type Rect struct {
	W int
	H int
}

func NewRect(w int, h int) Rect {
	return Rect{W: w, H: h}
}

func (r Rect) Area() int {
	return r.W * r.H
}
