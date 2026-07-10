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

// Code / Stamp (P4-C rider C6): named scalar types deliberately re-using a
// bare name main.go also declares (Code: int32 here vs int64 in main; Stamp:
// int64 here vs int32 in main), so the pkg_iface_samename_fmt_probe can pin
// the goo.fmt.<T>/goo.typedesc.<T> aliasing bug at interface_codegen.c's
// bare-name-keyed formatter cache: two same-named scalar types boxed into
// `any` and %v-formatted must each get their OWN width-correct loader, not
// silently share whichever one was emitted first (see the probe for the
// concrete before/after numbers).
type Code int32

func NewCode() Code {
	var c Code = 7
	return c
}

type Stamp int64

func NewStamp() Stamp {
	var s Stamp = 4294967406
	return s
}
