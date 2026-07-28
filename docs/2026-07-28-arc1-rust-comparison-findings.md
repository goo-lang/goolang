# Arc 1 — Goo measured against Rust and Go

**Date:** 2026-07-28
**Status:** findings. No compiler code changed.
**Harness:** `scripts/rust_compare_bench.sh`, programs in `bench/`.
**Revised:** 2026-07-28, same day. The first run of this pass used an unpinned
CPU and workloads that finished in under 0.1 s. At `/usr/bin/time`'s 10 ms
resolution that produced a reported 1.5x scalar deficit that DOES NOT EXIST.
Every number below is re-measured with cores pinned and workloads sized to run
for at least half a second. The retraction is kept in place, in Finding 5,
rather than quietly deleted.
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
| scalar, wall (1e9 iters) | **0.86 s** | 1.07 s | **0.86 s** | — |
| daemon, wall (1e6 reqs) | 2.24 s | 0.79 s | **0.55 s** | — |
| daemon, peak RSS | **1,627 MB** | 7.0 MB | **1.95 MB** | — |
| stencil, wall | 0.27 s | 0.29 s | 0.16 s | **0.10 s** |
| stencil, peak RSS | 18.6 MB | 19.5 MB | 18.3 MB | 18.1 MB |
| text, wall (1e6 lines) | 1.43 s | 0.50 s | **0.31 s** | — |
| text, peak RSS | **768 MB** | 6.7 MB | **1.95 MB** | — |
| hello, compile | 0.23 s | 0.10 s (warm) | 0.08 s | — |
| hello, binary | **106 KB** | 2,343 KB | 4,237 KB | — |

Wall clock is the median of seven runs, cores pinned (core 2 for the serial
benchmarks, cores 0-7 for the 8-lane stencil). Peak RSS is `/usr/bin/time -v`.

## Finding 1 — Goo does NOT beat Rust on the stencil. This overturns the premise.

The plan called parallel throughput "the axis Goo wins today", citing 7.55x
from `stencil-parallel-probe`.

**That 7.55x is Goo-8-lane against Goo-serial.** It is a self-comparison. It
says the lanes machinery scales; it says nothing whatever about Rust.

Measured against Rust on the identical algorithm and the identical 8-way
decomposition:

- Rust with `rayon` alone: **1.7x faster than Goo** (0.16 s against 0.27 s).
- Rust with `rayon` plus `std::simd`: **2.7x faster than Goo** (0.10 s).

Against **Go** it is a tie, not a win: 0.27 s against 0.29 s, which is inside
the run-to-run spread. The earlier 1.3x-over-Go reading came from the unpinned
measurement and does not survive. So the headline claim was not supported by
any comparison that included the competitor it named, and the consolation claim
against Go does not hold either.

**This answers the question Arc 1 was built to answer.** The plan asked whether
the stencil gap is thread-level or lane-level. It is **both**, and the ordering
matters: Rust is already ahead *before* SIMD enters, so Goo's per-lane scalar
throughput is behind on its own. Vector types alone will not close this.

## Finding 2 — the memory gap is worse than ADR 0002 recorded, because ADR 0002 compared against Go

ADR 0002 measured the daemon against Go and found 651 MB against 8 MB. Rust is
tighter than Go, so the gap against the actual competitor is larger:

- daemon, 1,000,000 requests: Goo 1,627 MB, Go 7.0 MB, **Rust 1.95 MB**. That
  is **834x Rust**, and it grows without bound while both others stay flat.
- text, 1,000,000 lines: Goo 768 MB, Rust 1.95 MB. That is **394x Rust**.

Goo is also **4.1x slower** than Rust in wall clock on the daemon and **4.6x
slower** on text. The allocation path costs throughput as well as memory. This
strengthens the case for ADR 0002 phase 2 rather than weakening it.

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

## Finding 5 — RETRACTED. Scalar code is at parity, and the original finding was a measurement artefact

**The first version of this document reported a 1.5x Goo-against-Rust deficit
on pure scalar code. That was wrong, and the error was in the method, not in
the compiler.**

What went wrong. The benchmark ran for 0.08 s. `/usr/bin/time -f %e` resolves
to 10 ms, so that is eight ticks, and the process was free to migrate across a
32-core box between turbo states. Goo's timings came out bimodal — 0.12 s five
times and 0.08 s four times — while Rust happened to land on 0.08 s every time.
Taking the median of a bimodal sample produced a clean-looking 1.5x that was
pure noise.

Re-measured with the core pinned and the workload scaled 10x:

| | wall |
|---|---|
| Goo | **0.86 s** |
| Rust | **0.86 s** |
| Go | 1.07 s |

**Goo is at exact parity with Rust on scalar code, and 1.24x faster than Go.**
The emitted IR says the same thing: LLVM unrolls the loop 5x into a tight
dependent chain with no memory traffic, which is what Rust also produces.

Two things follow. Goo's scalar code generation is not a problem and needs no
work. And every deficit that remains in this document is in the ALLOCATION
path or in parallel throughput, not in arithmetic — which is a much more
actionable place for them to be.

The methodology rules that came out of this are written into the header of
`scripts/rust_compare_bench.sh` so the mistake cannot be repeated silently.

## What this changes in the plan

1. **Arc 4 (memory) stays the top priority, and the case is much stronger.**
   834x against Rust on the daemon, and 4.1x slower in wall clock on the same
   program. The allocation path is both the memory problem AND the throughput
   problem.
2. **Arc 2 needs both halves, and `noalias` is no longer merely "the cheap
   half".** Rust wins before SIMD, so per-lane scalar throughput is the first
   problem. Vector types are still needed for the remaining 1.8x.
3. **New candidate arc: a package build cache.** Small, well-understood, and it
   turns compile speed from a loss into a win. Best effort-to-result ratio in
   this pass.
4. **Scalar parity is established.** No work needed, and it can be claimed.
5. **Retire the 7.55x claim in its current form.** It is a self-comparison. Say
   "lanes scale 7.55x against serial Goo", never "Goo beats Rust".

## Honest limits of this pass

- One machine. Cores are pinned and workloads are sized for at least half a
  second, but these are still indicative numbers, not certified ones.
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
