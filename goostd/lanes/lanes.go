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

import "far"

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

	// M2-B1: cross-rank collective wiring (zero-valued for in-process
	// Run). rank>0's lane 0 forwards RAW per-lane partials in local-ID
	// order over collSock; rank 0's lane 0 flat-combines in GLOBAL lane-ID
	// order over collSocks (never pre-combined per rank — float addition
	// is not associative, and bit-identity with the in-process scan
	// requires the identical accumulation sequence).
	isFar     bool
	rank      int
	world     int
	collSock  int
	collSocks []int
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

	// M2-B1 (Task 6): cross-rank collective wiring — see the matching Lane
	// field block's doc comment for the semantics; runCore copies these
	// onto every Lane unchanged.
	isFar     bool
	rank      int
	world     int
	collSock  int
	collSocks []int
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
				isFar:     fc.isFar,
				rank:      fc.rank,
				world:     fc.world,
				collSock:  fc.collSock,
				collSocks: fc.collSocks,
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

// farRecvMust wraps far.RecvF64 with an unconditional panic on any
// transport error — a torn transport is unrecoverable both mid-collective
// (allReduce's far branch, below) and mid-teardown (RunFar's marker
// receives), so both call sites share this one panic path. See allReduce's
// doc comment below for the collective's combine-order contract this
// helper participates in.
func farRecvMust(sock int) float64 {
	v := far.RecvF64(sock) catch e {
		panic("lanes: far recv failed: " + e.Error())
	}
	return v
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
//
// M2-B1 (Task 6): under RunFar (l.isFar), the collective becomes
// world-global with NO signature change. Rank 0's lane 0 still does the
// local ID-order combine first, then extends it with each remote rank's
// RAW per-lane partials — forwarded, never pre-combined, by that rank's
// own lane 0 — in ascending rank order, each rank's partials in ascending
// local-ID order. That is instruction-for-instruction the same
// accumulation sequence the in-process scan would produce for the same
// total lane count, which is the bit-identity contract this collective
// promises (float addition is not associative, so the order is
// load-bearing, not style).
func (l *Lane) allReduce(local float64, useMax bool) float64 {
	// Arc 16 fixed the compiler codegen gap this used to work around:
	// codegen_generate_index_expr now loads an lvalue index (e.g. the
	// struct-field selector l.id) before widening it, so l.id can be used
	// directly as an index again.
	l.partials[l.id] <- local
	if l.id == 0 {
		if !l.isFar {
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
		if l.rank == 0 {
			// GLOBAL flat combine, lane-ID order: rank 0's own lanes
			// first, then rank 1's raw partials, then rank 2's, ... —
			// instruction-for-instruction the in-process scan's sequence
			// for the same total lane count (bit-identity contract).
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
			for r := 1; r < l.world; r++ {
				for i := 0; i < l.count; i++ {
					v := farRecvMust(l.collSocks[r])
					if useMax {
						if v > acc {
							acc = v
						}
					} else {
						acc = acc + v
					}
				}
			}
			for r := 1; r < l.world; r++ {
				far.SendF64(l.collSocks[r], acc)
			}
			for i := 1; i < l.count; i++ {
				l.results[i] <- acc
			}
			return acc
		}
		// rank > 0's lane 0: forward RAW partials in local-ID order
		// (never pre-combined — see the Lane field block's comment),
		// then wait for the global result and distribute it locally.
		for i := 0; i < l.count; i++ {
			v := <-l.partials[i]
			far.SendF64(l.collSock, v)
		}
		acc := farRecvMust(l.collSock)
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

// farItoa: minimal non-negative int -> decimal string, local so lanes.go
// assumes nothing about vendored-to-vendored imports (only the far/fmt
// SHIM import path is proven). rank/world are small; negatives cannot
// reach it (RunFar validates first).
func farItoa(n int) string {
	if n == 0 {
		return "0"
	}
	digits := "0123456789"
	s := ""
	for n > 0 {
		d := n % 10
		s = digits[d:d+1] + s
		n = n / 10
	}
	return s
}

// RunFar is Run across process boundaries: `world` cooperating OS
// processes (ranks 0..world-1), each partitioning its own rank-local span,
// exchange halos over the far transport at rank boundaries. Interior lanes
// are wired exactly as Run wires them; only a rank's outermost lanes
// differ — their outward channels are bridged to NNG pair sockets by two
// pump goroutines per far edge (send-pump drains the cap-1 channel into
// far.SendF64; recv-pump feeds far.RecvF64 into the cap-1 channel), so
// Publish/HaloLeft/HaloRight/StencilStep bodies run unchanged and the M1
// cap-1 deadlock-freedom argument carries over (a send-pump's only job is
// draining the slot; far.SendF64 buffers without waiting on the remote).
// Global Dirichlet edges exist only at rank 0's left and rank world-1's
// right. Every rank must call RunFar with the SAME steps, world, urlBase,
// and per-rank lane count (equal-count is a documented contract; the
// bit-identity probes enforce it empirically).
//
// Teardown order (T6 review I1: marker exchange added on EVERY far
// socket, halo and coll, before any close). The sendDoneL/R drain
// (below) only protects against a send racing Close ON THIS RANK'S OWN
// enqueue step — it says nothing about whether the REMOTE rank has
// actually received what we sent. That gap is real and source-confirmed:
// far.SendF64/far.Close wrap NNG's nng_send/nng_close, and NNG's own
// nng_close(3) documents "closing the socket while data is in
// transmission will likely lead to loss of that data... there is no
// automatic linger or flush" — a message can sit in the local send
// buffer (NNG_OPT_SENDBUF, FAR_BUF_DEPTH=128) well after far.SendF64
// returns, and nni_msgq_close (traced in vendored NNG 1.11.0,
// src/core/msgqueue.c) unconditionally frees whatever is still queued
// there. Two concrete instances: rank 0's final collective broadcast
// (allReduce's `far.SendF64(l.collSocks[r], acc)`) and a rank's final
// halo Publish() are both susceptible — a peer still blocked reading
// either would get "far: closed" instead of the value.
//
// The fix: before touching any close, every far socket this rank holds
// exchanges a MARKER (0.0 — its FIFO position is the signal, not the
// value) with its peer: push the marker as this socket's truly-final
// send (halo: through sendL/sendR, so the still-running send-pump
// forwards it after any already-buffered final Publish; coll: direct
// far.SendF64, mirroring allReduce's own usage), then block receiving
// the peer's marker back (halo: one read of recvL/recvR — the BSP
// receive-every-round contract already guarantees nothing real is left
// unread there, so the next delivered value IS the peer's marker; coll:
// farRecvMust). Pushes for every socket happen first (both halo edges,
// then every coll socket) before any receive — see the exchange's own
// inline comment for why serializing edge-by-edge would still be
// deadlock-free but needlessly chains latency across a multi-rank halo
// run.
//
// What this proves, precisely: completing the exchange on a socket
// proves (via NNG's per-direction FIFO delivery) that THIS rank has now
// received every message its peer ever sent on that socket, including
// the peer's own marker. It does NOT, by itself, give either side a
// mathematical guarantee that ITS OWN last send (the marker) was
// received before it proceeds to close — with no linger primitive on
// either end of a symmetric, unconditional-push handshake, that residual
// question is a Two-Generals-shaped one no finite protocol eliminates
// outright. What the exchange DOES give: a full network round trip (send
// marker, peer receives it and independently sends its own, we receive
// that) has provably elapsed before either side reaches its close — the
// SAME "wait a while first" mitigation nng_close(3) itself recommends,
// now triggered by a real cross-rank event instead of a blind sleep, and
// applied on top of the existing local sendDoneL/R drain. This closes
// the window from "always exposed, sub-microsecond, on every far run" to
// "requires the local write aio to be starved for longer than a full
// round trip plus the rest of this rank's teardown". Chain-deadlock
// argument, summarized: every marker push this rank issues (both halo
// edges, then every coll socket) is unconditional and buffered, and ALL
// of a rank's pushes are issued before ANY of its receives — so no rank's
// marker receive is ever gated behind another rank's still-pending
// receive; the wait graph has no cycle, for any world size. Full
// derivation and the honest accounting of what remains unproven:
// docs/superpowers/specs/2026-07-24-p6-lanes-m2-b1-design.md,
// "Amendments (2026-07-24, execution round)".
//
// After the marker exchange: quit send-pumps and WAIT for each to
// confirm it has forwarded anything left in its channel and will touch
// its socket no more (see sendDoneL/R's doc comment below). Only THEN
// far.Close the halo sockets (which unblocks each recv-pump with the
// "far: closed" error they string-match), join the recv-pumps, then
// far.Close the coll sockets.
//
// Failure model: setup errors and mid-run transport errors panic with
// explicit messages (a torn transport is unrecoverable for lockstep BSP;
// same process-fatal story as a panicking lane body).
func RunFar(p Partitioned, steps int, rank int, world int, urlBase string, body func(ctx *Lane)) []float64 {
	if world < 1 {
		panic("lanes.RunFar: world must be >= 1")
	}
	if rank < 0 {
		panic("lanes.RunFar: rank out of range")
	}
	if rank >= world {
		panic("lanes.RunFar: rank out of range")
	}
	if len(urlBase) == 0 {
		panic("lanes.RunFar: urlBase must be non-empty")
	}
	hasL := rank > 0
	hasR := rank < world-1

	// Every rank LISTENS on its right boundary and DIALS its left, so
	// each boundary URL has exactly one owner. far.Dial is async-retry
	// (NNG_FLAG_NONBLOCK), so process start order cannot deadlock setup.
	sockL := 0
	sockR := 0
	if hasR {
		ur := urlBase + ".halo." + farItoa(rank)
		sr := far.Listen(ur) catch e {
			panic("lanes.RunFar: listen " + ur + ": " + e.Error())
		}
		sockR = sr
	}
	if hasL {
		ul := urlBase + ".halo." + farItoa(rank-1)
		sl := far.Dial(ul) catch e {
			panic("lanes.RunFar: dial " + ul + ": " + e.Error())
		}
		sockL = sl
	}

	fc := farCfg{}
	fc.hasL = hasL
	fc.hasR = hasR

	// M2-B1 (Task 6): collective socket wiring. Rank 0 LISTENS one socket
	// per remote rank (collSocks[r]); rank r>0 DIALS its single socket to
	// rank 0 (collSock). Set up before the pump block so it's in place
	// before any lane goroutine can call AllReduceSum/AllReduceMax.
	fc.isFar = world > 1
	fc.rank = rank
	fc.world = world
	if world > 1 {
		if rank == 0 {
			collSocks := make([]int, world)
			for r := 1; r < world; r++ {
				uc := urlBase + ".coll." + farItoa(r)
				sc := far.Listen(uc) catch e {
					panic("lanes.RunFar: listen " + uc + ": " + e.Error())
				}
				collSocks[r] = sc
			}
			fc.collSocks = collSocks
		}
		if rank > 0 {
			uc := urlBase + ".coll." + farItoa(rank)
			sc := far.Dial(uc) catch e {
				panic("lanes.RunFar: dial " + uc + ": " + e.Error())
			}
			fc.collSock = sc
		}
	}

	quitL := make(chan int, 1)
	quitR := make(chan int, 1)
	// sendDoneL/sendDoneR are a SEPARATE completion signal from pumpDone,
	// one per send-pump, and RunFar blocks on them before calling far.Close
	// (see the teardown block below).
	//
	// Drop-freedom invariant (T4 review round — does NOT assume anything
	// about select's arm-evaluation policy): by the time RunFar sends
	// quitL/quitR, runCore has already returned, which means every lane
	// goroutine has already joined — so the PRODUCER of sendL/sendR values
	// (the kernel's Publish() calls) no longer exists. Whatever the kernel's
	// last Publish() enqueued (at most ONE value, since sendL/sendR are
	// cap-1 and FIFO) is therefore already sitting in the channel, in full,
	// by the time the send-pump's quit arm runs — nothing can ARRIVE after
	// that point to be missed. So each send-pump's quit arm does one
	// non-blocking drain of its channel (a `select`+`default`, below) before
	// signaling sendDoneL/R: if a value is there, forward it; if not, the
	// pump was already caught up. This makes drop-freedom a property of
	// producer-quiescence + a bounded, FIFO, single-producer channel —
	// independent of whether the outer select happens to scan arms in
	// source order, uniformly at random, or by any other policy a future
	// select rework might choose. RunFar blocking on sendDoneL/R before
	// far.Close then orders Close strictly after this drain, so the
	// underlying transport (whose send path panics on any nonzero nng_send
	// return, including the ECLOSED a same-process Close would raise) never
	// races an in-flight far.SendF64.
	sendDoneL := make(chan int, 1)
	sendDoneR := make(chan int, 1)
	pumpDone := make(chan int, 2)
	recvPumps := 0
	if hasL {
		sendL := make(chan float64, 1)
		recvL := make(chan float64, 1)
		fc.sendL = sendL
		fc.recvL = recvL
		go func() {
			for {
				select {
				case v := <-sendL:
					far.SendF64(sockL, v)
				case <-quitL:
					// Drain: at most one value can be queued (cap-1), and
					// no producer exists anymore — RunFar sends quit only
					// AFTER every lane joined, so anything ever queued is
					// already in the channel when we drain (see the
					// invariant comment above). One non-blocking attempt
					// is sufficient, and order-independent of the outer
					// select's own arm-scan policy.
					select {
					case v := <-sendL:
						far.SendF64(sockL, v)
					default:
					}
					sendDoneL <- 1
					return
				}
			}
		}()
		go func() {
			for {
				v := far.RecvF64(sockL) catch e {
					if e.Error() == "far: closed" {
						pumpDone <- 1
						return
					}
					panic("lanes.RunFar: far recv failed: " + e.Error())
				}
				recvL <- v
			}
		}()
		recvPumps = recvPumps + 1
	}
	if hasR {
		sendR := make(chan float64, 1)
		recvR := make(chan float64, 1)
		fc.sendR = sendR
		fc.recvR = recvR
		go func() {
			for {
				select {
				case v := <-sendR:
					far.SendF64(sockR, v)
				case <-quitR:
					// Drain: mirrors sendL's quit arm above — at most one
					// value can be queued (cap-1) and no producer exists
					// anymore, so one non-blocking attempt is sufficient
					// and order-independent of the outer select's own
					// arm-scan policy.
					select {
					case v := <-sendR:
						far.SendF64(sockR, v)
					default:
					}
					sendDoneR <- 1
					return
				}
			}
		}()
		go func() {
			for {
				v := far.RecvF64(sockR) catch e {
					if e.Error() == "far: closed" {
						pumpDone <- 1
						return
					}
					panic("lanes.RunFar: far recv failed: " + e.Error())
				}
				recvR <- v
			}
		}()
		recvPumps = recvPumps + 1
	}

	out := runCore(p, steps, body, fc)

	// M2-B1 (T6 review I1 fix): teardown marker exchange, BEFORE any quit
	// or close — see RunFar's doc comment above for the full argument.
	// Push phase: every socket this rank holds gets its marker pushed
	// FIRST (both halo edges, then every coll socket) — not push-then-
	// immediately-wait one socket at a time. Deadlock/latency
	// re-derivation (see RunFar's doc comment above, and the spec's
	// "Amendments (2026-07-24, execution round)" section, for the full
	// per-rank-chain trace): a per-edge push is always unconditional here
	// (never gated on having received anything first), so pushing both
	// edges up front means EVERY rank's halo pushes fire independently of
	// every other rank's progress — the round trip each recv below waits
	// on resolves in one hop for every rank simultaneously. Serializing
	// instead (push L, recv L, push R, recv R) would still terminate
	// (rank 0's boundary push is unconditional with no left neighbor to
	// wait on, so the R-marker wave ripples rightward and always
	// completes — no cycle), just with latency proportional to a rank's
	// distance from the nearest boundary instead of O(1); overlapping the
	// pushes avoids that chain entirely, so it's what's implemented.
	if hasL {
		fc.sendL <- 0.0
	}
	if hasR {
		fc.sendR <- 0.0
	}
	if world > 1 {
		if rank == 0 {
			for r := 1; r < world; r++ {
				far.SendF64(fc.collSocks[r], 0.0)
			}
		}
		if rank > 0 {
			far.SendF64(fc.collSock, 0.0)
		}
	}
	// Receive phase: the peer's marker on every socket this rank holds.
	// Halo: a single read of recvL/recvR — the BSP receive-every-round
	// contract already guarantees nothing real is left unread there (see
	// runCore's per-lane HaloLeft/HaloRight contract), so the next
	// delivered value IS the peer's marker. Coll: farRecvMust, matching
	// allReduce's own error-handling discipline (a torn transport
	// mid-teardown is exactly as unrecoverable as one mid-collective).
	if hasL {
		<-fc.recvL
	}
	if hasR {
		<-fc.recvR
	}
	if world > 1 {
		if rank == 0 {
			for r := 1; r < world; r++ {
				farRecvMust(fc.collSocks[r])
			}
		}
		if rank > 0 {
			farRecvMust(fc.collSock)
		}
	}

	// Teardown order (review-round fix): quit send-pumps THEN WAIT for each
	// one's completion ack BEFORE closing its socket — see sendDoneL/R's
	// doc comment above for why the wait is load-bearing, not defensive
	// padding. Only once both send-pumps are provably done touching their
	// sockets is it safe to Close, which then unblocks each recv-pump
	// (still legitimately blocked in far.RecvF64, per the BSP protocol's
	// receive-every-round contract) with the "far: closed" error they
	// string-match. The marker exchange above has already run by this
	// point, so this Close is no longer the FIRST event that could race
	// an undelivered send (see RunFar's doc comment for what that does
	// and doesn't prove).
	if hasL {
		quitL <- 1
		<-sendDoneL
	}
	if hasR {
		quitR <- 1
		<-sendDoneR
	}
	if hasL {
		far.Close(sockL)
	}
	if hasR {
		far.Close(sockR)
	}
	k := 0
	for k < recvPumps {
		<-pumpDone
		k = k + 1
	}

	// M2-B1 (Task 6, T6 review I1 fix): close collective sockets AFTER the
	// halo pumps have joined AND after the marker exchange above has
	// already confirmed this rank received every marker its coll peers
	// sent. Collectives make direct blocking calls from lane goroutines
	// (no pump goroutines involved), so there is nothing to quit here.
	if world > 1 {
		if rank == 0 {
			for r := 1; r < world; r++ {
				far.Close(fc.collSocks[r])
			}
		}
		if rank > 0 {
			far.Close(fc.collSock)
		}
	}
	return out
}
