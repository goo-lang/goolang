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
	step     int // current-round counter; advanced by Step() each BSP round
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

	// M2-B2 (Task 1): deterministic-collective wiring. count is the total
	// lane count (needed for the ID-order combine); partials[i] carries lane
	// i's contribution to the combiner (lane 0); results[i] carries the
	// combined value back to lane i (i >= 1; lane 0 keeps its own). All
	// cap-1, wired by Run before any goroutine spawns, like the halo
	// channels. Every lane holds the full slices, but lane i only ever
	// sends on partials[i] / receives on results[i], and only lane 0 reads
	// partials[*] / sends results[*].
	count    int
	partials []chan float64
	results  []chan float64

	// M2-B2 (Task 2): per-lane kernel buffers. scratch (width-sized) is
	// wired by Run — v1 has no memory reclamation, so per-round make()
	// inside a kernel would leak steps*width floats; one buffer per lane
	// for the whole Run is the discipline. haloBufL/haloBufR are lazily
	// sized by StencilStep on first call (radius isn't known to Run) —
	// one allocation per lane lifetime, then reused every round.
	scratch  []float64
	haloBufL []float64
	haloBufR []float64
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
	return runCore(p, steps, body, farCfg{})
}

// farCfg carries RunFar's process-boundary wiring into runCore. The zero
// value (all false/nil) is exactly Run's M1/M2-B2 behavior: process edges
// are global Dirichlet edges. M2-B1 design record:
// docs/superpowers/specs/2026-07-24-p6-lanes-m2-b1-design.md.
type farCfg struct {
	hasL  bool
	hasR  bool
	sendL chan float64
	recvL chan float64
	sendR chan float64
	recvR chan float64
}

// runCore is Run's body, parameterized by farCfg so RunFar (Task 4) can
// bridge a rank's outermost lanes to process-boundary channels while every
// interior lane is wired identically to Run. Equivalence argument: with the
// zero farCfg (fc.hasL == fc.hasR == false), edgeL == (i==0) and
// edgeR == (i==count-1) exactly as before, so `i > 0 ⇔ !edgeL` and
// `i < count-1 ⇔ !edgeR` — the exact conditions the pre-Task-4 wiring used —
// making Run(p, steps, body) == runCore(p, steps, body, farCfg{}) bit-for-bit.
func runCore(p Partitioned, steps int, body func(ctx *Lane), fc farCfg) []float64 {
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

	// M2-B2 (Task 1): collective channels — one partial + one result slot
	// per lane, cap 1 each. Sequential AllReduce calls stay correctly
	// paired without extra synchronization because each channel is
	// single-writer single-reader and FIFO: lane i's round-N+1 partial
	// cannot be sent until its round-N result was received.
	partials := make([]chan float64, p.count)
	results := make([]chan float64, p.count)
	for i := 0; i < p.count; i++ {
		partials[i] = make(chan float64, 1)
		results[i] = make(chan float64, 1)
	}

	for i := 0; i < p.count; i++ {
		i := i // per-iteration rebind: capture THIS i, not the shared loop var
		go func() {
			l := Lane{
				id:       i,
				steps:    steps,
				own:      p.backing[i*p.width : (i+1)*p.width],
				edgeL:    i == 0 && !fc.hasL,
				edgeR:    i == p.count-1 && !fc.hasR,
				count:    p.count,
				partials: partials,
				results:  results,
				scratch:  make([]float64, p.width),
			}
			if i > 0 {
				l.recvL = rightward[i-1]
				l.sendL = leftward[i-1]
			}
			if i == 0 && fc.hasL {
				l.sendL = fc.sendL
				l.recvL = fc.recvL
			}
			if i < p.count-1 {
				l.sendR = rightward[i]
				l.recvR = leftward[i]
			}
			if i == p.count-1 && fc.hasR {
				l.sendR = fc.sendR
				l.recvR = fc.recvR
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

// allReduce is the shared collective core: every lane contributes `local`;
// the combined value is returned identically to every lane. Combination
// happens in FIXED lane-ID order (lane 0's partial, then 1's, ...): lane 0
// drains partials[0..count-1] in index order and broadcasts on
// results[1..count-1]. ID order — never arrival order — is what makes the
// result bit-identical across runs and schedules for a given lane count
// (float addition is not associative, so this is load-bearing, not style).
// Bit-identity across DIFFERENT lane counts is deliberately not promised:
// a different tiling changes the rounding sequence.
//
// The call is also a barrier: no lane returns until lane 0 has combined
// every contribution. Deadlock-freedom: every send targets a cap-1 channel
// whose single buffered slot is empty at that point in the round (each
// channel is used exactly once per collective, single-writer,
// single-reader), so no send ever blocks on a peer that is itself blocked
// sending — the same argument as Publish's cap-1 reasoning above.
//
// PROTOCOL RULE (documented, not enforced): collectives are whole-round
// events. Every lane must call the same collective the same number of
// times, in the same program position — all lanes or none, like the
// Publish -> HaloLeft/HaloRight -> compute order.
func (l *Lane) allReduce(local float64, useMax bool) float64 {
	// Arc 16 fixed the compiler codegen gap this used to work around:
	// codegen_generate_index_expr now loads an lvalue index (e.g. the
	// struct-field selector l.id) before widening it, so l.id can be used
	// directly as an index again.
	l.partials[l.id] <- local
	if l.id == 0 {
		acc := <-l.partials[0]
		for i := 1; i < l.count; i++ {
			v := <-l.partials[i]
			if useMax {
				if v > acc {
					acc = v
				}
			} else {
				acc = acc + v
			}
		}
		for i := 1; i < l.count; i++ {
			l.results[i] <- acc
		}
		return acc
	}
	r := <-l.results[l.id]
	return r
}

// AllReduceSum returns the ID-order sum of every lane's `local`.
func (l *Lane) AllReduceSum(local float64) float64 {
	return l.allReduce(local, false)
}

// AllReduceMax returns the maximum of every lane's `local` (ID-order scan;
// max is order-insensitive but the fixed order keeps one code path).
func (l *Lane) AllReduceMax(local float64) float64 {
	return l.allReduce(local, true)
}

// StencilStep runs ONE BSP round of a (2*radius+1)-tap stencil over the
// lane's own tile: halo exchange of `radius` boundary cells per side, then
// out[i] = sum_k coeffs[k] * in[i+k-radius] (k ascending — the fixed
// accumulation order the bit-identity differentials pin), computed into the
// per-lane scratch buffer and copied back. Callers drive the round loop
// with ctx.Step(), exactly like a hand-written M1 body.
//
// radius is COMPTIME: each distinct radius monomorphizes its own instance
// (goo_pkg__lanes__StencilStep__n<radius>) in which every loop bound below
// is a constant — that is the entire specialization payoff (unrolled taps,
// hoisted coefficient loads, vectorizable inner loop at -O2). A package
// function, not a Lane method: comptime parameters are still walled on
// methods (src/types/type_checker.c "not yet supported on methods").
//
// Halo protocol for radius >= 2: the M1 channels are cap-1 and carry one
// float per send, so a radius-r exchange runs r sequential SUB-EXCHANGES.
// Sub-exchange k ships each lane's k-th-from-edge cell (own[width-1-k]
// rightward, own[k] leftward) and receives the neighbors' counterparts;
// each sub-exchange is exactly M1's proven both-sends-then-both-receives
// cap-1 pattern, so the per-step deadlock-freedom argument applies to each
// k in turn (no lane starts sub-exchange k+1 before finishing k, because
// its own receives for k gate it). haloBufL[k] holds the cell k+1 to the
// left of own[0]; haloBufR[k] the cell k+1 to the right of own[width-1].
// Edge sides fill from ctx.boundary (Dirichlet 0.0, M1-frozen).
//
// len(coeffs) must be exactly 2*radius+1; anything else is a programming
// error and panics (explicit, never silent).
//
// Fix round (M2-B2 Task 4, boundary peel): the tap-accumulation sweep below
// is split into three loops instead of one dispatching sweep over all of
// `own`. Reason: with a single loop, every tap access carried the full
// idx<0 / idx>=w halo-boundary dispatch, live inside the innermost loop for
// every cell — including the vast interior majority where neither branch
// can ever be taken. Splitting the sweep gives the interior loop
// (radius <= i < w-radius) a tap body that is provably in-bounds by
// construction — i-radius >= 0 and i+radius <= w-1 hold for every i in that
// range — so the halo dispatch is dropped entirely there (not merely
// never-taken), leaving a branch-free accumulation. The two boundary loops
// (i < radius, and i >= max(radius, w-radius)) keep the original full
// dispatch body unchanged, since only cells within `radius` of a tile edge
// can ever actually read a halo buffer. The right loop's start is clamped
// to max(radius, w-radius) rather than always starting at w-radius: when
// radius <= w < 2*radius the tile is narrower than two radii, so a plain
// w-radius start would fall short of radius and re-walk (double-compute)
// cells the left loop already wrote; the clamp makes the two boundary loops
// meet or abut with no gap and no overlap in that case (verified for
// w == 2*radius and radius <= w < 2*radius). Each loop's accumulation is
// textually identical to the original per-cell body (`acc := 0.0` then
// k-ascending `acc = acc + coeffs[k]*v`) — no reassociation, no hoisting of
// the coefficient multiply — so this is a pure control-flow restructuring,
// not a numeric one: the golden/differential bit-identity contract is
// unaffected.
//
// Measured result (-O2, goo_pkg__lanes__StencilStep__n2, see
// task-4-report.md's fix-round IR evidence): the interior loop's dispatch
// is confirmed gone in the IR, but none of the three tap loops unrolls or
// vectorizes even so — each remains a genuine loop (3 total `fmul double`,
// one per loop, zero vector types). Two independent blockers survive the
// peel: (1) `goo_bounds_check` calls remain on every tap access (both
// `own[...]` and `coeffs[k]`) with no readnone/speculatable attributes, and
// (2) this specialized instance's radius is still passed as a runtime i64
// parameter rather than folded to the literal 2 in the function body — so
// even the interior loop's trip count is not a compile-time constant to
// LLVM despite the symbol being per-radius-mangled. (2) is a codegen fact
// about how comptime parameters lower today, not something a library-level
// loop restructuring can address; branch removal alone was necessary but
// not sufficient here.
func StencilStep(ctx *Lane, comptime radius int, coeffs []float64) {
	if len(coeffs) != 2*radius+1 {
		panic("lanes.StencilStep: len(coeffs) must equal 2*radius+1")
	}
	// A radius wider than the tile would need cells beyond the immediate
	// neighbor's tile (the sub-exchange protocol only reaches one neighbor
	// deep) and would index haloBufL/haloBufR out of bounds — reject
	// explicitly rather than corrupt (T2 review, Minor 3).
	if radius > len(ctx.own) {
		panic("lanes.StencilStep: radius must not exceed the lane tile width")
	}
	if len(ctx.haloBufL) < radius {
		ctx.haloBufL = make([]float64, radius)
	}
	if len(ctx.haloBufR) < radius {
		ctx.haloBufR = make([]float64, radius)
	}
	own := ctx.own
	w := len(own)

	for k := 0; k < radius; k++ {
		if !ctx.edgeR {
			ctx.sendR <- own[w-1-k]
		}
		if !ctx.edgeL {
			ctx.sendL <- own[k]
		}
		if ctx.edgeL {
			ctx.haloBufL[k] = ctx.boundary
		} else {
			ctx.haloBufL[k] = <-ctx.recvL
		}
		if ctx.edgeR {
			ctx.haloBufR[k] = ctx.boundary
		} else {
			ctx.haloBufR[k] = <-ctx.recvR
		}
	}

	// Left boundary: only cells within `radius` of the left tile edge can
	// ever read a halo buffer, so this keeps the full dispatch body.
	for i := 0; i < radius; i++ {
		acc := 0.0
		for k := 0; k <= 2*radius; k++ {
			idx := i + k - radius
			v := 0.0
			if idx < 0 {
				v = ctx.haloBufL[(0-idx)-1]
			} else {
				if idx >= w {
					v = ctx.haloBufR[idx-w]
				} else {
					v = own[idx]
				}
			}
			acc = acc + coeffs[k]*v
		}
		ctx.scratch[i] = acc
	}
	// Interior: branch-free by construction. i-radius >= 0 and
	// i+radius <= w-1 hold for every i in [radius, w-radius), so idx is
	// always in [0, w) and no halo read is ever reachable — the dispatch
	// is dropped, not merely never-taken.
	for i := radius; i < w-radius; i++ {
		acc := 0.0
		for k := 0; k <= 2*radius; k++ {
			acc = acc + coeffs[k]*own[i+k-radius]
		}
		ctx.scratch[i] = acc
	}
	// Right boundary: same full dispatch body as the left loop. Start is
	// clamped to max(radius, w-radius) — not always w-radius — so that when
	// radius <= w < 2*radius (tile narrower than two radii) this loop picks
	// up exactly where the left loop left off instead of re-walking cells
	// the left loop already computed.
	start := w - radius
	if start < radius {
		start = radius
	}
	for i := start; i < w; i++ {
		acc := 0.0
		for k := 0; k <= 2*radius; k++ {
			idx := i + k - radius
			v := 0.0
			if idx < 0 {
				v = ctx.haloBufL[(0-idx)-1]
			} else {
				if idx >= w {
					v = ctx.haloBufR[idx-w]
				} else {
					v = own[idx]
				}
			}
			acc = acc + coeffs[k]*v
		}
		ctx.scratch[i] = acc
	}
	for i := 0; i < w; i++ {
		own[i] = ctx.scratch[i]
	}
}
