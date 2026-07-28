# 3. Competitive position, measured against Rust

Date: 2026-07-28
Status: **proposed**

## Context

The stated goal is "beat Rust across the board while keeping Go's ease of use".

Until 2026-07-28 no benchmark in this repository had ever measured Rust. The
goal named a competitor nobody had run. `docs/2026-07-28-arc1-rust-comparison-
findings.md` closes that hole with five programs in three languages, every row
asserted to produce identical output before any timing is reported.

**Three of the working assumptions did not survive.**

### Assumption 1: "Goo wins parallel throughput." It does not.

The claim rested on `stencil-parallel-probe`'s 7.55x. That figure is Goo-8-lane
against **Goo-serial** — a self-comparison. On the identical algorithm and the
identical 8-way decomposition, Rust with `rayon` is 1.25x faster than Goo, and
Rust with `rayon` plus `std::simd` is 2.3x faster.

Goo does beat Go by 1.3x on the same program. That is real. It is not the claim
that was being made.

### Assumption 2: "Compile speed, probably winning." Half right, for the wrong reason.

Hello-world: Goo 0.32 s against rustc 0.09 s — apparently a loss. The breakdown
says otherwise. Goo's own pipeline is **0.05 s**, faster than rustc. The other
0.27 s is recompiling `goostd/strings` from source, because
`src/package/import_resolver.c` has no package build cache. Go's warm 0.03 s is
a cache result: Go cold takes 2.64 s.

### Assumption 3: "Reach parity on memory." The gap is bigger than recorded.

ADR 0002 measured the daemon against Go. Rust is tighter than Go. Against the
actual competitor the daemon is **163x** (326 MB against 2.0 MB), and Goo is
also 3.9x slower in wall clock on that program — the allocation path costs
throughput, not only memory.

### One thing nobody had claimed turns out to be a large win

Binary size. Hello-world: Goo **106 KB**, Go 2,343 KB, Rust 4,237 KB. That is
22x smaller than Go and 40x smaller than Rust, and it falls out of having no
garbage collector and a small runtime.

## Decision

**Stop claiming "beat Rust across the board". Claim a narrower position that
the measurements support, and state the concessions.**

The proposed position:

> Goo gives Go's spelling with deterministic memory, compile-time-proven
> parallel safety, and a fraction of the binary size. It does not give Rust's
> peak throughput, and it does not give Rust's ecosystem.

Each axis gets one of three commitments, and nothing is claimed without a probe
that measures it against Rust:

| Axis | Commitment | Basis |
|---|---|---|
| Binary size | **Win, and say so** | 22x Go, 40x Rust, measured |
| Ease of use | **Win, and say so** | No lifetimes. 26 of 130 stdlib functions would need annotations in Rust |
| Compile speed | **Win, after a package cache** | Core pipeline 0.05 s already beats rustc 0.09 s |
| Parallel safety | **Win, and say so** | Compile-time proven disjointness, zero annotations. This is about the PROOF, never about the throughput |
| Deterministic memory | **Parity target** | ADR 0002. Currently 163x behind Rust |
| Parallel throughput | **Parity target, not a win** | Currently 1.25x behind rayon, 2.3x behind rayon plus SIMD |
| Scalar throughput | **Parity target** | 1.5x behind on pure arithmetic, cause unknown |
| Ecosystem | **Concede openly** | 17 files, 3,533 lines |

The distinction that carries the whole position: **Goo competes with Rust on
the PROOF of parallel safety, not on parallel speed.** Rust proves disjointness
through the borrow checker and charges lifetime annotations for it. Goo proves
it in `lane_ownership.c` and charges nothing. That claim is true today and does
not depend on winning a benchmark.

## Why not the alternatives

**Keep claiming "across the board" — rejected.** The measurements contradict it
on three axes. A claim a reader can disprove in ten minutes with the harness in
this repository costs more credibility than the claim buys.

**Claim only what wins today and drop the parity targets — rejected.** It would
mean dropping memory, which is the axis ADR 0002 exists to fix and the one that
decides whether a long-running service is expressible at all. A parity target
is an honest commitment, not a weaker claim.

**Pivot away from Rust to compete only with Go — rejected, but it is the
closest call.** Goo beats Go on the stencil and on binary size today, and the
Go-compatibility investment points that way. It is rejected because the memory
model would then have no forcing function: against Go, a garbage collector is
sufficient, and ADR 0002 already recorded why that ends the argument for using
Goo at all.

## Consequences

### Positive

- Every claim gets a probe that measures the competitor named in it.
- The `7.55x` figure gets stated correctly, which removes a claim that would
  not survive an informed reader.
- Binary size becomes a claim, having been an unmeasured accident.
- The compile-speed work gets a target and a known cause.

### Negative, and named rather than minimised

- The project loses its headline. "Beats Rust at numerics" was load-bearing in
  how this work was described, and it was not true.
- Parity on parallel throughput is not obviously reachable. Rust wins before
  SIMD enters, so this is not one missing feature.
- The scalar 1.5x is unexplained. Until it is understood, no throughput claim is
  safe, because it sits underneath every other benchmark.

## What this changes in the work order

1. **ADR 0002 memory work stays first.** The case is stronger, not weaker.
2. **`noalias` from the lane-ownership proof comes before vector types.** Rust
   wins pre-SIMD, so per-lane scalar throughput is the first problem. This also
   hands LLVM exactly the information Rust's borrow checker hands it.
3. **New arc: a package build cache** in `src/package/import_resolver.c`. Best
   effort-to-result ratio found in this pass.
4. **New investigation: the scalar 1.5x**, before any throughput claim.

## Open, and deliberately not decided here

- Whether to keep pursuing parallel-throughput parity, or to concede throughput
  and compete purely on the safety proof plus ergonomics.
- Whether binary size is promoted to a gated property, with a probe that fails
  when hello-world exceeds a threshold.
- Whether the harness ever becomes a gate. It reports today, on purpose: a
  timing threshold in `verify-core` would fail on a smaller machine and teach
  people to ignore a red gate.
