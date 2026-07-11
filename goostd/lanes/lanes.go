// goostd/lanes (P6 M1 Task 3 + Task 4): compile-time-partitioned
// data-parallel lanes with BSP halo exchange. Partition splits a []float64
// into `count` equal-width disjoint tiles. Run spawns one goroutine per
// lane, each operating on its own disjoint sub-slice view into the shared
// backing array plus a pair of cap-1 boundary channels to each neighbor,
// runs body against it, and joins every one of them before returning.
//
// The Partition/Run/Own/ID struct and function shapes are FROZEN (v1,
// Task 3) — do not rename or reshape fields. Task 4 added the four
// halo/BSP methods (Publish/HaloLeft/HaloRight/Step) below, wired the
// send*/recv*/edge*/boundary fields Run populates per lane, and left body's
// call count unchanged (Run still calls body exactly once — see Run's doc
// comment for how body itself drives the multi-round BSP loop via Step()).
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

// Lane is the per-goroutine execution context `body` runs against. Field
// shapes are frozen (Task 3); Run (Task 4) now populates send*/recv*/
// edge*/boundary per lane before calling body, and Publish/HaloLeft/
// HaloRight/Step (below) are the only code that reads/writes them.
// `boundary` is left at its zero value (0.0) — there is no setter, so
// every lane's missing-neighbor value is the Dirichlet constant 0.0; a
// non-zero boundary would need a new Partition/Run parameter, which is out
// of scope (the v1 API surface is frozen).
type Lane struct {
	id       int
	steps    int // total rounds requested (Run's `steps` param, frozen for Task 4's BSP loop)
	step     int // current-round counter; unused in v1 (Run calls body exactly once)
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
//
// count must evenly divide len(arr): width is floor(len(arr)/count), so a
// non-divisor count silently drops the tail len(arr)%count elements from
// every lane's view — they are never written by Run.
func Partition(arr []float64, comptime count int) Partitioned {
	w := len(arr) / count
	return Partitioned{backing: arr, count: count, width: w}
}

// Run spawns one goroutine per lane, each handed a Lane pointing at its own
// disjoint sub-slice of p.backing, runs body against it, and joins every
// lane (draining `count` receives) before returning. No halos: lanes never
// touch each other's data in this v1.
//
// Task 4 (BSP halo exchange): Run wires one pair of cap-1 channels per
// adjacent lane boundary BEFORE spawning any goroutine — rightward[i] carries
// lane i's right boundary to lane i+1 (i's Publish send, i+1's HaloLeft
// recv); leftward[i] carries lane i+1's left boundary to lane i (i+1's
// Publish send, i's HaloRight recv). Capacity 1 is load-bearing: see
// Lane.Publish's doc comment for why capacity 0 deadlocks. Edge lanes (id 0
// and id count-1) simply have no channel on their missing side — sendL/recvL
// (lane 0) and sendR/recvR (lane count-1) stay nil, and edgeL/edgeR gate
// Publish/HaloLeft/HaloRight away from ever touching a nil channel.
//
// v1 called body exactly once per lane; Task 4 leaves that contract
// unchanged — Run still calls body exactly once — but body is now expected
// to itself drive the BSP loop by calling ctx.Step() (which returns false
// once `steps` rounds have elapsed):
//
//	body := func(ctx *lanes.Lane) {
//		for {
//			ctx.Publish()
//			hl := ctx.HaloLeft()
//			hr := ctx.HaloRight()
//			// ... compute this step from hl/hr/ctx.Own() into a scratch
//			// buffer, then copy back (see Lane.Step's doc comment) ...
//			if !ctx.Step() {
//				break
//			}
//		}
//	}
//
// This keeps Run itself unchanged (one call, one goroutine, one join per
// lane) and puts all step-count control in the body/Lane pair, which is the
// natural place for it since Step()'s false-after-steps semantics are a
// per-lane concern, not a per-Run one.
//
// An unrecovered panic inside body terminates the whole process (exit(2) via
// goo_panic, src/runtime/runtime.c:143-149), not just the offending lane —
// there is no per-goroutine recover boundary here.
func Run(p Partitioned, steps int, body func(ctx *Lane)) []float64 {
	done := make(chan int, p.count)

	// Per-adjacent-pair boundary channels, built before any goroutine spawns
	// so every lane's send/recv fields are wired from data that already
	// exists when its goroutine starts reading them (no channel is ever
	// mutated after this loop). count-1 pairs for count lanes; count<=1 skips
	// this loop entirely (no neighbors to wire).
	rightward := make([]chan float64, p.count)
	leftward := make([]chan float64, p.count)
	for i := 0; i < p.count-1; i++ {
		rightward[i] = make(chan float64, 1)
		leftward[i] = make(chan float64, 1)
	}

	for i := 0; i < p.count; i++ {
		i := i // per-iteration rebind: capture THIS i, not the shared loop var
		go func() {
			l := Lane{
				id:    i,
				steps: steps,
				own:   p.backing[i*p.width : (i+1)*p.width],
				edgeL: i == 0,
				edgeR: i == p.count-1,
			}
			if !l.edgeR {
				l.sendR = rightward[i]
			}
			if !l.edgeL {
				l.recvL = rightward[i-1]
			}
			if !l.edgeL {
				l.sendL = leftward[i-1]
			}
			if !l.edgeR {
				l.recvR = leftward[i]
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

// Publish sends this lane's current boundary cells to its neighbors: the
// rightmost owned cell on sendR (received by the right neighbor's
// HaloLeft), the leftmost owned cell on sendL (received by the left
// neighbor's HaloRight). Edge lanes skip the missing side (sendL/sendR are
// nil there and Run never wires a send on a nil channel by construction —
// see Run's edgeL/edgeR gating).
//
// Capacity-1 channels are load-bearing here: within one step BOTH neighbors
// call Publish (each sending on the channel the other is about to receive
// from) before either calls HaloLeft/HaloRight. A capacity-0 (unbuffered)
// channel would block the first Publish's send until the paired recv is
// ready, but the paired lane is itself blocked in its own Publish send at
// that moment — classic two-party deadlock. Capacity 1 lets both sends
// complete immediately (into the channel's one-deep buffer), so both lanes
// then proceed to their receives, which drain what was just buffered. See
// docs/superpowers/specs/2026-07-11-p6-lanes-m1-design.md Component 3 for
// the full per-step deadlock-freedom argument.
func (l *Lane) Publish() {
	if !l.edgeR {
		l.sendR <- l.own[len(l.own)-1]
	}
	if !l.edgeL {
		l.sendL <- l.own[0]
	}
}

// HaloLeft returns the left neighbor's published right-boundary value: the
// fixed Dirichlet boundary for the leftmost lane (edgeL), otherwise a
// blocking receive on recvL. The received value is also cached in l.haloL
// for callers that want it without re-reading (HaloLeft/HaloRight are each
// called at most once per step in the documented protocol, so this is
// idempotent-in-practice rather than load-bearing).
func (l *Lane) HaloLeft() float64 {
	if l.edgeL {
		return l.boundary
	}
	l.haloL = <-l.recvL
	return l.haloL
}

// HaloRight returns the right neighbor's published left-boundary value:
// the fixed Dirichlet boundary for the rightmost lane (edgeR), otherwise a
// blocking receive on recvR.
func (l *Lane) HaloRight() float64 {
	if l.edgeR {
		return l.boundary
	}
	l.haloR = <-l.recvR
	return l.haloR
}

// Step advances the BSP round counter and reports whether another round
// should run: it increments l.step, then returns true while l.step is still
// below the `steps` total Run was called with. Callers drive the whole BSP
// loop from this return value (see Run's doc comment for the canonical
// `for { ...; if !ctx.Step() { break } }` shape) — Step never runs the
// compute itself, it only counts rounds.
//
// Off-by-one is deliberately pinned by the 2-step failing test from Task
// 4's Step 1: step starts at 0 (zero value), so the first Step() call
// (after step 0's publish/halo/compute has already happened) increments it
// to 1 and returns 1 < 2 == true, running a second round; the second
// Step() call increments to 2 and returns 2 < 2 == false, stopping after
// exactly `steps` rounds total.
func (l *Lane) Step() bool {
	l.step = l.step + 1
	return l.step < l.steps
}
