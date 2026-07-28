# bench/ — Goo measured against Rust and Go

Driven by `scripts/rust_compare_bench.sh`. Findings in
`docs/2026-07-28-arc1-rust-comparison-findings.md`. Strategic conclusions in
`docs/adr/0003-competitive-position-measured-against-rust.md`.

## The rule these programs follow

Every language version of a benchmark implements the **same algorithm** and
prints the **same output**. The harness asserts that agreement before it reports
any timing, because a row where the outputs differ is not a measurement — it
means the programs are not the same computation.

The Goo file carries the full rationale for each benchmark. The Go and Rust
files point back at it rather than repeating it.

| Directory | Measures |
|---|---|
| `scalar/` | Raw backend quality. A dependent 64-bit LCG chain, no allocation, no vectorisation possible. |
| `daemon/` | Retention. Allocate per request, hold nothing. Peak RSS is the number that matters. |
| `stencil/` | Parallel throughput. 1<<20 cells, 1000 rounds, 8 lanes. Goo uses `goostd/lanes`, Go uses goroutines plus a WaitGroup barrier, Rust uses `rayon`. |
| `text/` | The runtime helper allocation path — `append`, `Fields`, `Join`, `ToUpper` — which ADR 0002 phase 2 owns. |
| `hello/` | Compile time and binary size. Not run time. |

## Fairness decisions, recorded so they can be argued with

- **`rayon` is pinned to 8 threads.** It defaults to one worker per logical CPU,
  which is 32 on the measurement box. The algorithm is defined as eight lanes and
  the Goo and Go versions use eight, so pinning measures the same decomposition
  rather than rewarding Rust for a wider default.
- **`std::simd` is nightly-only and is reported as its own row.** A Goo number
  compared against it is compared against what Rust can do, not against what Rust
  ships to users on stable. Both matter, and they are different claims.
- **The Rust and Go versions are idiomatic translations, not zero-allocation
  rewrites.** They build the same Vecs, maps and Strings the Goo version builds.
  The difference under measurement is what happens to them afterwards.
- **The same author wrote all three versions.** A Rust specialist would likely
  make the Rust faster, so the reported gaps are probably optimistic for Goo.

## Running

```bash
bash scripts/rust_compare_bench.sh            # all five
bash scripts/rust_compare_bench.sh stencil    # one
RUNS=9 bash scripts/rust_compare_bench.sh     # more samples
```

Reports, never gates. Deliberately not in `verify-core`: a timing threshold
there would fail on a smaller machine and teach people to ignore a red gate.
