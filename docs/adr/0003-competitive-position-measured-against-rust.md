# 3. Competitive position, measured against Rust

Date: 2026-07-28
Status: **accepted** (2026-07-28)

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
identical 8-way decomposition, Rust with `rayon` is **1.7x** faster than Goo,
and Rust with `rayon` plus `std::simd` is **2.7x** faster.

Against Go it is a tie (0.27 s against 0.29 s), not the 1.3x win an earlier,
unpinned measurement suggested. So the consolation claim does not hold either.

### Assumption 2: "Compile speed, probably winning." Half right, for the wrong reason.

Hello-world was 0.23 s against rustc's 0.08 s — apparently a loss. The cause
was not re-parsing, and not a missing package cache. Imported-package functions
carried EXTERNAL linkage, so LLVM's `globaldce` could not remove them however
unreachable they were: a program calling `strings.Repeat` once dragged all of
`goostd/strings` and `goostd/utf8` through the -O2 pipeline and into the object.

Internal linkage is the correct linkage — Goo compiles a whole program into one
module and has no separate compilation. **Fixed: hello-world now compiles in
0.05 s against rustc's 0.07 s and Go's 0.11 s, the fastest of the three**, and
the stencil binary fell from 534 KB to 108 KB. Go's warm 0.11 s is a cache
result: Go cold takes 2.64 s.

### Assumption 3: "Reach parity on memory." The gap is bigger than recorded.

ADR 0002 measured the daemon against Go. Rust is tighter than Go. Against the
actual competitor, at 1,000,000 requests, the daemon is **834x** (1,627 MB
against 1.95 MB), and Goo is also **4.1x slower in wall clock** on that same
program. The allocation path is the memory problem AND the throughput problem.

### One correction this ADR must carry, because it was published wrong

The first version of the findings reported a 1.5x scalar deficit against Rust.
It was a measurement artefact — an unpinned CPU and an 0.08 s workload against
a 10 ms timer. Re-measured properly, **Goo and Rust are at exact parity on
scalar code (0.86 s each), and Goo is 1.24x faster than Go.**

That correction matters to the position below. Goo's code generation for
ordinary arithmetic is not a weakness, so every remaining deficit sits in the
allocation path or in parallel throughput — both of which have owners.

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
| Compile speed | **Win, established** | 0.05 s against rustc 0.07 s and Go 0.11 s |
| Parallel safety | **Win, and say so** | Compile-time proven disjointness, zero annotations. This is about the PROOF, never about the throughput |
| Deterministic memory | **Parity target** | ADR 0002. Currently 834x behind Rust |
| Parallel throughput | **Parity target, not a win** | Currently 1.7x behind rayon, 2.7x behind rayon plus SIMD |
| Scalar throughput | **Win over Go, parity with Rust — established** | 0.86 s against Rust's 0.86 s and Go's 1.07 s |
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
- Binary size becomes a claim, having been an unmeasured accident — and it
  improved again when the linkage fix landed (stencil 534 KB to 108 KB).
- Compile speed is now a measured win rather than a target.

### Negative, and named rather than minimised

- The project loses its headline. "Beats Rust at numerics" was load-bearing in
  how this work was described, and it was not true.
- Parity on parallel throughput is not obviously reachable. Rust wins before
  SIMD enters, so this is not one missing feature.
- The allocation path is 4x slower AND 800x heavier. Those are probably the same
  root cause, but ADR 0002 phase 2 has to prove that rather than assume it.

## What this changes in the work order

1. **ADR 0002 memory work stays first.** The case is stronger, not weaker.
2. **`noalias` from the lane-ownership proof comes before vector types.** Rust
   wins pre-SIMD, so per-lane scalar throughput is the first problem. This also
   hands LLVM exactly the information Rust's borrow checker hands it.
3. **DONE.** Compile speed was fixed by internal linkage for imported packages,
   not by a package build cache. The cache proposal is withdrawn: it addressed a
   cost that was never there.
4. **No scalar work is needed.** Parity is established and can be claimed.

## Open, and deliberately not decided here

- Whether to keep pursuing parallel-throughput parity, or to concede throughput
  and compete purely on the safety proof plus ergonomics.
- Whether binary size is promoted to a gated property, with a probe that fails
  when hello-world exceeds a threshold.
- Whether the harness ever becomes a gate. It reports today, on purpose: a
  timing threshold in `verify-core` would fail on a smaller machine and teach
  people to ignore a red gate.
