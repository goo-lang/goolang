# Arc 1 — Goo measured against Rust and Go

**Date:** 2026-07-28
**Status:** findings. No compiler code changed.
**Harness:** `scripts/rust_compare_bench.sh`, programs in `bench/`.
**Machine:** 32 cores, Linux. `goo` v0.1.0, `go1.26.1`, `rustc 1.96.0` stable
(plus nightly for `std::simd`).

## Why this pass existed

The stated goal is to beat Rust. Nothing in this repository had ever measured
Rust. Every priority in the plan rested on a guess about where the gap was.

Five programs, three languages, identical algorithms. The harness asserts that
every language prints the **same output** before it reports any timing — a row
where the outputs differ is not a measurement, it means the programs are not
the same computation. All five rows agree.

## The numbers

| Benchmark | Goo | Go | Rust | Rust + SIMD |
|---|---|---|---|---|
| scalar, wall | 0.12 s | 0.10 s | **0.08 s** | — |
| daemon, wall | 0.43 s | 0.11 s | **0.11 s** | — |
| daemon, peak RSS | **326 MB** | 7.5 MB | **2.0 MB** | — |
| stencil, wall | 0.30 s | 0.39 s | 0.24 s | **0.13 s** |
| stencil, peak RSS | 17.7 MB | 19.3 MB | 18.0 MB | 17.3 MB |
| text, wall | 0.27 s | 0.13 s | **0.08 s** | — |
| text, peak RSS | **154 MB** | 6.8 MB | **1.9 MB** | — |
| hello, compile | 0.32 s | 0.13 s (warm) | 0.09 s | — |
| hello, binary | **106 KB** | 2,343 KB | 4,237 KB | — |

Wall clock is the median of five runs. Peak RSS is `/usr/bin/time -v`.

## Finding 1 — Goo does NOT beat Rust on the stencil. This overturns the premise.

The plan called parallel throughput "the axis Goo wins today", citing 7.55x
from `stencil-parallel-probe`.

**That 7.55x is Goo-8-lane against Goo-serial.** It is a self-comparison. It
says the lanes machinery scales; it says nothing whatever about Rust.

Measured against Rust on the identical algorithm and the identical 8-way
decomposition:

- Rust with `rayon` alone: **1.25x faster than Goo** (0.24 s against 0.30 s).
- Rust with `rayon` plus `std::simd`: **2.3x faster than Goo** (0.13 s).

Goo does beat **Go** by 1.3x (0.30 s against 0.39 s), which is real and worth
keeping. But the headline claim as written was not supported by any comparison
that included the competitor it named.

**This answers the question Arc 1 was built to answer.** The plan asked whether
the stencil gap is thread-level or lane-level. It is **both**, and the ordering
matters: Rust is already ahead *before* SIMD enters, so Goo's per-lane scalar
throughput is behind on its own. Vector types alone will not close this.

## Finding 2 — the memory gap is worse than ADR 0002 recorded, because ADR 0002 compared against Go

ADR 0002 measured the daemon against Go and found 651 MB against 8 MB. Rust is
tighter than Go, so the gap against the actual competitor is larger:

- daemon, 200,000 requests: Goo 326 MB, Go 7.5 MB, **Rust 2.0 MB**. That is
  **163x Rust**.
- text: Goo 154 MB, Rust 1.9 MB. That is **81x Rust**.

Goo is also 3.9x slower than Rust in wall clock on the daemon and 3.4x slower
on text — so the allocation path costs throughput as well as memory, not only
memory. This strengthens the case for ADR 0002 phase 2 rather than weakening
it.

## Finding 3 — the compile-speed loss is not what it looks like, and it is cheap to fix

The headline says Goo compiles hello-world in 0.32 s against rustc's 0.09 s.
That reading is wrong, and the breakdown matters:

| What | Time |
|---|---|
| `goo --emit-llvm` (parse, check, IR) | **0.02 s** |
| `gcc` link against `libgoo_runtime.a` | 0.02 s |
| Program importing only `fmt` (a C shim) | **0.05 s** |
| Program importing `strconv` (415 vendored lines) | 0.17 s |
| Program importing `strings` (630 vendored lines) | 0.32 s |

Compile time tracks **which packages are imported**, not source size. A
23-line program with no vendored import compiles in 0.05 s; a 13-line
hello-world that imports `strings` takes 0.32 s.

The cause: **there is no package build cache.** `src/package/import_resolver.c`
contains no caching logic, so every build recompiles every vendored `goostd`
package from source, at roughly 0.4 ms per vendored line.

So the true position is the opposite of the headline: **Goo's compiler is fast
— 0.05 s, faster than rustc's 0.09 s on the same shape.** It is paying 0.27 s
to rebuild `goostd/strings` on every single invocation. Go's warm-cache 0.03 s
is a cache result, not raw speed: Go with a cold cache takes **2.64 s**.

This is the cheapest win identified in this pass, and it converts a measured
loss into a measured win.

## Finding 4 — binary size is a large, unclaimed win

| | hello binary |
|---|---|
| Goo | **106 KB** |
| Go | 2,343 KB (22x Goo) |
| Rust | 4,237 KB (40x Goo) |

Nobody had measured this and nothing claims it. For containers, embedded
targets and cold-start latency it is a real advantage, and it comes free from
having no garbage collector and a small runtime.

## Finding 5 — an unexplained 1.5x on pure scalar code

The scalar benchmark is a dependent 64-bit LCG chain. No allocation, no memory
access, no vectorisation possible, same LLVM backend in Goo and Rust. Goo
should be at parity.

It is not: 0.12 s against 0.08 s, a 1.5x deficit on arithmetic alone.

Nothing in this pass explains it. It is NOT nil checks or bounds checks — the
loop touches no pointer and no slice. This needs its own investigation, because
a 1.5x penalty on scalar code sits underneath every other benchmark here and
would cap any parallel win.

## What this changes in the plan

1. **Arc 4 (memory) stays the top priority, and the case is stronger.** 163x
   against Rust, and the allocation path costs throughput too.
2. **Arc 2 needs both halves, and `noalias` is no longer merely "the cheap
   half".** Rust wins before SIMD, so per-lane scalar throughput is the first
   problem. Vector types are still needed for the remaining 1.8x.
3. **New candidate arc: a package build cache.** Small, well-understood, and it
   turns compile speed from a loss into a win. Best effort-to-result ratio in
   this pass.
4. **New open question: the scalar 1.5x.** Investigate before claiming any
   performance parity.
5. **Retire the 7.55x claim in its current form.** It is a self-comparison. Say
   "lanes scale 7.55x against serial Goo", never "Goo beats Rust".

## Honest limits of this pass

- One machine, one run of the harness. The numbers are indicative, not
  certified.
- Five programs is a narrow sample. There is no I/O benchmark, no map-heavy
  benchmark, and no long-running-service benchmark.
- The Rust and Go versions are idiomatic translations written by the same
  author as the Goo versions. A Rust specialist would likely make the Rust
  faster, not slower, so the gaps reported here are probably **optimistic for
  Goo**.
- `std::simd` is nightly-only. The Rust-SIMD row is what Rust can do, not what
  Rust ships on stable. Both are reported for that reason.

## Reproducing

```bash
bash scripts/rust_compare_bench.sh            # all five
bash scripts/rust_compare_bench.sh stencil    # one
RUNS=9 bash scripts/rust_compare_bench.sh     # more samples
```

The harness reports and never gates. It is deliberately NOT in `verify-core`: a
timing threshold there would fail on a smaller machine and teach people to
ignore a red gate.
