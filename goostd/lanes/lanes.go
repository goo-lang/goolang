// goostd/lanes v1 (P6 M1 Task 3): compile-time-partitioned data-parallel
// lanes. Partition splits a []float64 into `count` equal-width disjoint
// tiles (no halos yet — Task 4 adds boundary exchange). Run spawns one
// goroutine per lane, each operating on its own disjoint sub-slice view
// into the shared backing array, and joins every one of them before
// returning.
//
// The struct/function shapes below are FROZEN for Task 4 — do not rename
// or reshape fields; halo/BSP methods land there, unused here.
//
// Vendored-Goo language-gap workarounds (documented in-file, per the
// goostd/strconv convention):
//   - No combined `var a, b = ...`: every binding is its own `:=`/`var`.
//   - Tagged switch on rune is bugged (tagless only) — not used in this
//     file; noted for anyone extending it.
//   - Fixed-size arrays cannot be sliced; only real slices (declared
//     []float64, not [N]float64) support `s[i:j]` — Partitioned.backing
//     and Lane.own are both real slices for exactly this reason.
//   - Loop-variable capture by a goroutine/closure is REJECTED by the
//     checker. Run uses the proven `i := i` per-iteration rebind idiom
//     (examples/closure_probe.goo), and that rebind MUST live inside a
//     classic 3-clause `for i := 0; i < p.count; i++ { ... }` header, not
//     a tagless `for cond { ...; i = i+1 }` loop — in the tagless form the
//     rebind's shadow would intercept the increment statement and the
//     outer loop variable would never advance.
package lanes

// Partitioned describes an equal-width tiling of `backing` into `count`
// disjoint lanes of `width` elements each.
type Partitioned struct {
	backing []float64
	count   int
	width   int
}

// Lane is the per-goroutine execution context `body` runs against. The
// halo*/send*/recv*/edge*/boundary fields are frozen for Task 4's BSP halo
// exchange and are unused (left zero-valued) by this v1 — Partition/Run
// never read or write them.
type Lane struct {
	id       int
	steps    int
	step     int
	own      []float64
	haloL    float64
	haloR    float64
	sendL    chan float64
	sendR    chan float64
	recvL    chan float64
	recvR    chan float64
	edgeL    bool
	edgeR    bool
	boundary float64
}

// Partition splits arr into `count` equal-width tiles. count is a comptime
// param (Task 2's package-function comptime lift), so w's division is
// constant arithmetic specialized per instance. The body reads only its own
// params (arr, count) — no package-level globals — per the comptime
// package-function body-scoping limitation.
func Partition(arr []float64, comptime count int) Partitioned {
	w := len(arr) / count
	return Partitioned{backing: arr, count: count, width: w}
}

// Run spawns one goroutine per lane, each handed a Lane pointing at its own
// disjoint sub-slice of p.backing, runs body against it, and joins every
// lane (draining `count` receives) before returning. No halos: lanes never
// touch each other's data in this v1.
func Run(p Partitioned, steps int, body func(ctx *Lane)) []float64 {
	done := make(chan int, p.count)
	for i := 0; i < p.count; i++ {
		i := i // per-iteration rebind: capture THIS i, not the shared loop var
		go func() {
			l := Lane{
				id:    i,
				steps: steps,
				own:   p.backing[i*p.width : (i+1)*p.width],
			}
			body(&l)
			done <- i
		}()
	}
	j := 0
	for j < p.count {
		<-done
		j = j + 1
	}
	return p.backing
}

// Own returns this lane's disjoint slice view into the shared backing array.
func (l *Lane) Own() []float64 {
	return l.own
}

// ID returns this lane's index (0..count-1).
func (l *Lane) ID() int {
	return l.id
}
