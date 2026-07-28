// Package sort sorts slices and user-defined collections.
//
// The Interface form is the whole design. Go's `sort.Slice(x any, less func)`
// is implemented with reflection, which Goo does not have, so it is NOT
// provided — and no Goo-specific replacement is invented in its place. Sorting
// something other than the three provided slice types means implementing
// Len/Less/Swap, exactly as Go's own sort.Sort requires.
//
// ALGORITHM: heapsort. O(n log n) worst case, in place, no recursion, and no
// pathological input. Go uses pdqsort with a heapsort fallback for depth. Like
// Go's sort.Sort, this is NOT stable — equal elements may be reordered. Go
// documents the same guarantee, so relying on stability from sort.Sort is a bug
// in either language; sort.Stable is the API that promises it, and it is not in
// this cut.
package sort

// Interface is the same three methods Go requires.
type Interface interface {
	Len() int
	Less(i int, j int) bool
	Swap(i int, j int)
}

// siftDown restores the heap invariant for the subtree rooted at lo, treating
// hi as one past the last live element.
func siftDown(data Interface, lo int, hi int) {
	root := lo
	for {
		child := 2*root + 1
		if child >= hi {
			return
		}
		// Pick the larger of the two children before comparing with the root.
		if child+1 < hi && data.Less(child, child+1) {
			child = child + 1
		}
		if !data.Less(root, child) {
			return
		}
		data.Swap(root, child)
		root = child
	}
}

// Sort sorts data in ascending order as determined by its Less method.
func Sort(data Interface) {
	n := data.Len()
	// Guard rather than relying on (n-1)/2 for n == 0. Truncation direction for
	// a negative dividend is a language detail this code has no need to depend
	// on, and an empty or single-element collection is already sorted.
	if n < 2 {
		return
	}
	// Build the heap bottom-up, then repeatedly move the max to the end.
	for i := (n - 1) / 2; i >= 0; i-- {
		siftDown(data, i, n)
	}
	for i := n - 1; i > 0; i-- {
		data.Swap(0, i)
		siftDown(data, 0, i)
	}
}

// IsSorted reports whether data is already in ascending order.
func IsSorted(data Interface) bool {
	n := data.Len()
	for i := n - 1; i > 0; i-- {
		if data.Less(i, i-1) {
			return false
		}
	}
	return true
}

// reverse wraps an Interface and inverts its Less.
//
// This is Go's exact spelling: an EMBEDDED interface, which promotes Len and
// Swap automatically so only Less has to be written. Declaring Less on the
// outer type SHADOWS the promoted one, which is the whole mechanism.
type reverse struct {
	Interface
}

// The whole point: arguments swapped. Len and Swap are promoted from the
// embedded Interface and dispatch to whatever it holds.
func (r reverse) Less(i int, j int) bool { return r.Interface.Less(j, i) }

// Reverse returns data with its ordering inverted, for use with Sort.
func Reverse(data Interface) Interface {
	return reverse{Interface: data}
}

// IntSlice attaches Interface to []int.
//
// A named slice type with methods, which is Go's exact shape.
type IntSlice []int

func (x IntSlice) Len() int               { return len(x) }
func (x IntSlice) Less(i int, j int) bool { return x[i] < x[j] }
func (x IntSlice) Swap(i int, j int)      { x[i], x[j] = x[j], x[i] }

// StringSlice attaches Interface to []string.
type StringSlice []string

func (x StringSlice) Len() int               { return len(x) }
func (x StringSlice) Less(i int, j int) bool { return x[i] < x[j] }
func (x StringSlice) Swap(i int, j int)      { x[i], x[j] = x[j], x[i] }

// Float64Slice attaches Interface to []float64.
//
// Go orders NaN before every other value so that the result is a total order.
// This does the same: `a != a` is true only for NaN.
type Float64Slice []float64

func (x Float64Slice) Len() int { return len(x) }

func (x Float64Slice) Less(i int, j int) bool {
	a := x[i]
	b := x[j]
	if a < b {
		return true
	}
	return a != a && b == b
}

func (x Float64Slice) Swap(i int, j int) { x[i], x[j] = x[j], x[i] }

// Ints sorts a slice of ints in ascending order.
func Ints(x []int) {
	Sort(IntSlice(x))
}

// Strings sorts a slice of strings in ascending byte order.
func Strings(x []string) {
	Sort(StringSlice(x))
}

// Float64s sorts a slice of float64s in ascending order, NaNs first.
func Float64s(x []float64) {
	Sort(Float64Slice(x))
}

// IntsAreSorted reports whether x is in ascending order.
func IntsAreSorted(x []int) bool {
	return IsSorted(IntSlice(x))
}

// StringsAreSorted reports whether x is in ascending order.
func StringsAreSorted(x []string) bool {
	return IsSorted(StringSlice(x))
}

// Float64sAreSorted reports whether x is in ascending order.
func Float64sAreSorted(x []float64) bool {
	return IsSorted(Float64Slice(x))
}

// SearchInts returns the smallest index at which x could be inserted into a
// sorted slice a and keep it sorted. It returns len(a) when every element is
// smaller than x. a MUST already be sorted; the result is meaningless if not.
func SearchInts(a []int, x int) int {
	lo := 0
	hi := len(a)
	for lo < hi {
		// lo + (hi-lo)/2 rather than (lo+hi)/2, which can overflow for very
		// large slices. Free here, and it removes a footgun for a reader who
		// copies this loop somewhere hotter.
		mid := lo + (hi-lo)/2
		if a[mid] < x {
			lo = mid + 1
		} else {
			hi = mid
		}
	}
	return lo
}
