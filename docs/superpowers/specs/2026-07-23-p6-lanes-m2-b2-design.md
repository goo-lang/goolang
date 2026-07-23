# Phase 6 lanes M2-B2: comptime-specialized numeric kernels — design

Date: 2026-07-23. Status: approved (user, this date). Successor to
`2026-07-11-p6-lanes-m1-design.md` (M1, merged as PR #187 → dd11713).
Decomposition position: step 3 of the Phase 6 ladder (0 spike → 1 comptime
metaprogramming → 2 shared-mem lane harness → **3 comptime-specialized
kernel** → 4 far-transport).

## Goals

1. Ship the specialization payoff the decomposition promised: kernels whose
   shape is comptime-known monomorphize into stride-fixed, unrolled,
   vectorizable loops — proven by IR, not asserted by prose.
2. Lift M1's no-reductions limit safely: a deterministic BSP collective
   (fixed combination order → schedule-independent bit-identity), which
   unlocks the canonical scientific loop — iterate + convergence check
   (Jacobi-style).

## API additions (`goostd/lanes`)

Three surfaces; `Partition`/`Run` and the BSP halo protocol are unchanged.

### 1. `lanes.StencilStep(ctx *Lane, comptime radius int, coeffs []float64)`

One BSP step (Publish → HaloLeft/HaloRight → compute) with the tap loop
specialized on `radius`.

- **Package function, not a `Lane` method**: M1 lifted the
  comptime-param wall for package functions only; methods may still be
  walled. Planning must verify the method wall's current state — the
  design does not depend on lifting it.
- **`coeffs` is a slice**, not `[2*radius+1]float64`: fixed-size arrays
  cannot cross a function signature boundary (known M1 trap). With
  `radius` comptime the inner loop still unrolls to `2*radius+1` taps and
  LLVM hoists the coefficient loads at -O2. Length mismatch
  (`len(coeffs) != 2*radius+1`) is a runtime error at entry (explicit,
  not silent).
- Update rule: `out[i] = Σ coeffs[k] * in[i+k-radius]` over the lane's own
  tile, halo cells supplying the boundary reads — the same
  associativity-safe per-element shape M1's bit-identity argument covers.

### 2. `ctx.AllReduceSum(local float64) float64` / `ctx.AllReduceMax(local float64) float64`

BSP collectives on the existing `Lane` ctx (plain methods — no comptime
params needed, so the method wall is irrelevant here):

- Every lane contributes `local`; every lane receives the identical
  combined result. The call is also a barrier (like `Step`): no lane
  proceeds until the result is available to all.
- `Sum` covers residual/L2-style norms; `Max` covers max-norm convergence.
  No user-supplied combiner in this milestone (a closure combiner would
  reopen the determinism argument per-user; the two fixed ops keep it
  closed).

### 3. Determinism model (load-bearing)

Partials are combined in **fixed lane-ID order** (lane 0's partial, then
lane 1's, …), never arrival order:

- **Promised**: bit-identical results across runs and schedules for a
  given lane count. Same discipline as M1's differential backstop.
- **Explicitly NOT promised**: bit-identity across *different* lane
  counts — float addition is non-associative; changing the tiling changes
  the rounding sequence. Documented in `docs/lanes.md`'s M2 section.
- Implementation: per-lane partial channels gathered by a designated
  combiner (lane 0), combined in ID order, result broadcast on per-lane
  result channels. Capacity-1 channels, goroutine-parking — the same
  primitives as the halo protocol; no new runtime machinery.

## Checker / codegen impact — deliberately near-zero

- Comptime monomorphization of package functions already works (M1:
  `goo_pkg__lanes__StencilStep__n<radius>` falls out of the existing
  mangle path).
- The lane-ownership pass (`src/types/lane_ownership.c`) should need no
  new obligations: collectives neither create nor leak views. Planning
  must verify one risk: passing `ctx *Lane` to a package function inside
  a `Run` body must not trip obligation 3/4 attribution. If it does, the
  fix is a scoped extension to the pass's selector classification
  (recognize `lanes.StencilStep` as lane-ctx-preserving), not a redesign.
- No parser/grammar changes. Grammar tripwire stays 31/0.

## Testing / gates (M1's pattern, extended)

- **Golden differential probes** (mandatory, default gate):
  1. Specialized stencil vs an M1-style unspecialized body: bit-identical
     output on the same input/lane count.
  2. Jacobi-with-convergence (StencilStep + AllReduceMax loop) parallel
     vs a sequential reference that mimics the same tiled ID-order
     combination: bit-identical, including identical iteration count.
- **IR pin** (sibling of `lanes-monomorphize-ir-pin`): the specialized
  symbol exists AND its -O2 IR contains vector types (`<N x double>`) —
  proving the payoff, not just the plumbing.
- **Reject fixtures** for whatever ownership rule planning confirms (at
  minimum: the existing five partition/run obligations still hold with
  the new API in play).
- **Benchmark**: extend `stencil-parallel-probe`; wall-clock speedup
  assertion stays opt-in (`LANES_BENCH_ASSERT_SPEEDUP`), per M1's
  decision that timing asserts don't belong in the default gate.

## Non-goals

- 2D/higher-dimensional partitions (own milestone).
- Generic `[T]` kernels — deferred with the type-system milestone
  (`Numeric` constraint / instance-time operator checking,
  `docs/spmd-harness.md` "Limits").
- Bit-identity across lane counts (see Determinism model).
- User-supplied reduction combiners.
- NNG / far transport (B1 — sequenced after B2 by the recorded decision).
- Transitive comptime forwarding (`Run` still cannot forward a comptime
  `count` inward; `radius` is written literally at the call site).

## Risks & open questions for planning

1. Comptime-param method wall: confirmed walled? (Design assumes yes;
   package function chosen accordingly.)
2. Ownership pass vs package-function ctx passing (see above).
3. Vectorization pin robustness: `<N x double>` presence at -O2 may vary
   by LLVM version/target — pin the loosest predicate that still proves
   vectorization (e.g. any `x double>` vector type in the specialized
   function body).
4. `AllReduce*` under `steps`-loop nesting: the barrier must not deadlock
   against `Step()`'s parking — planning should trace the channel
   protocol for a lane that calls `AllReduceSum` while a neighbor is in
   `Step()` (protocol-order rule may need documenting: collectives are
   whole-round events, called by all lanes or none — same fixed-order
   discipline as Publish→Halo).
