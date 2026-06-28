# Scheduler Default-Flip — Parallelism-by-Default (Design)

**Date:** 2026-06-29
**Status:** Approved (brainstorming complete)
**Branch:** `feat/m11-scheduler-default-flip`
**Predecessor:** M9 channel-deadlock detection (PR #34, merged `f8d4455`). This is the
"payoff" milestone that all M8a prerequisites (M8c MT-scheduler, M8d yield/channel
MT-hygiene, M9 deadlock detection) were built to enable.

---

## 1. Goal

Flip Goo's goroutine scheduler from **single-threaded-by-default** to **true
parallelism-by-default**, Go-faithfully. Today a program that spawns goroutines
lazily initializes the scheduler with exactly one OS worker thread
(`goo_scheduler_init(1)` at `src/runtime/concurrency.c:175`). After this milestone the
default worker count is derived from the hardware (online CPU count), overridable via
the `GOMAXPROCS` environment variable, matching Go's default runtime behavior.

## 2. Scope

### In scope
1. **Default thread-count resolution.** Replace the hardcoded `goo_scheduler_init(1)`
   lazy-init with a computed default:
   - `GOMAXPROCS` env var if set to a valid integer `>= 1`, else
   - online CPU count via `sysconf(_SC_NPROCESSORS_ONLN)`,
   - clamped to `[1, GOO_MAX_OS_THREADS]` (cap stays **16**).
2. **A named resolver function** `goo_default_thread_count()` (single responsibility,
   side-effect-free, unit-testable) so the policy is not buried inline in `goo_go`.
3. **Two soak probes** exercising codegen-emitted goroutine programs under real
   parallelism, looped to catch nondeterministic interleavings:
   - `parallel-soak-probe` — channel fan-in.
   - `parallel-select-soak-probe` — select-heavy multiplexing.
4. **CI pinned to `GOMAXPROCS=4`** for a reproducible gate — pinned to **4, not 1**, so
   the entire existing `verify:` suite runs genuinely multi-threaded (pinning to 1 would
   give near-zero coverage of the flip).

### Out of scope (YAGNI)
- A programmatic `runtime.GOMAXPROCS(n)` setter API (env var + lazy default suffices).
- Work-stealing / scheduler tuning, per-goroutine CPU affinity.
- Raising the `GOO_MAX_OS_THREADS = 16` cap.

## 3. Architecture

The behavioral change is mechanically tiny; the policy is isolated into its own
function for testability and call-site clarity.

```c
// src/runtime/concurrency.c — new helper. Single responsibility: resolve the
// default OS-thread count from environment and hardware. No side effects.
static int goo_default_thread_count(void) {
    // 1. GOMAXPROCS env override (Go-faithful): honored only if a valid int >= 1.
    goo_string_t env = goo_os_getenv("GOMAXPROCS");
    if (env.data != NULL && env.len > 0) {
        int n = /* parse env.data[0..len) as a base-10 int */;
        if (n >= 1) return (n > GOO_MAX_OS_THREADS) ? GOO_MAX_OS_THREADS : n;
        // n < 1 or unparseable -> fall through to NCPU (Go ignores invalid GOMAXPROCS)
    }
    // 2. Online CPU count, clamped to [1, cap].
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) ncpu = 1;                       // sysconf failure -> safe floor
    return (ncpu > GOO_MAX_OS_THREADS) ? GOO_MAX_OS_THREADS : (int)ncpu;
}
```

The only call-site change:

```c
goo_goroutine_t* goo_go(goo_goroutine_func_t func, void* arg) {
    if (!g_scheduler) {
        goo_scheduler_init(goo_default_thread_count());  // was: goo_scheduler_init(1)
    }
    ...
}
```

### Why this shape
- `goo_scheduler_init()` is **unchanged** — it already clamps to the cap and normalizes
  `<= 0` to 1. We only change *what we pass it*.
- **Explicit-init callers are unaffected.** The C stress tests call
  `goo_scheduler_init(4)` directly, bypassing the lazy path, so `mt-scheduler-stress`,
  `chan-mt-stress`, and `yield-stress` keep gating exactly as before.
- `GOMAXPROCS` invalid / `<= 0` / non-numeric falls back to NCPU, matching Go's
  documented behavior.

### Data flow (unchanged from M8c/M8d)
N worker threads pull from the shared `ready_queue`; per-thread `_Thread_local` state
(`t_current` / `t_reap` / `t_requeue`) isolates each worker; the deadlock poll loop in
`goo_scheduler_wait` counts *goroutines* (not threads) so it is N-independent.

## 4. Verification strategy

**Risk model:** the hand-written C stress tests exercise the scheduler core at N=4, but
not the **codegen-emitted goroutine path** (`.goo` -> LLVM -> binary) under parallelism.
A latent race in channel send/recv glue, select wakeup, or escape-promoted stack slots
would surface only when real compiled programs run multi-threaded. The soak probes close
that gap.

### Three-layer gate
1. **Whole-suite-goes-parallel (regression).** With CI pinned to `GOMAXPROCS=4`, the
   entire existing `verify:` suite (~40 probes) now runs genuinely multi-threaded. Any
   probe that silently relied on single-thread ordering fails here. Expected: all still
   green; a failure is a real latent bug the flip exposed.
2. **`parallel-soak-probe` (fan-in).** A deliberately race-prone `.goo` program — many
   goroutines feeding one shared channel into a collector — with a deterministic expected
   result (e.g. sum = N*(N-1)/2). The Makefile target runs the compiled binary in a loop
   (50 iterations); **any** iteration with wrong output or nonzero exit fails the probe.
3. **`parallel-select-soak-probe` (select-heavy).** Goroutines multiplexing across
   several channels via blocking `select`, with a deterministic total. Exercises the
   select wakeup path specifically (distinct from fan-in), also looped 50x.

Both soak probes also serve as **deadlock-false-positive regression checks at N>1**:
their goroutines always make progress, so they must never trip the
`goo_scheduler_wait` stability gate.

### Unit check
A small test asserts `goo_default_thread_count()` honors `GOMAXPROCS=1`, `GOMAXPROCS=8`,
`GOMAXPROCS=99 -> clamped to 16`, and `GOMAXPROCS=garbage -> NCPU`. Pure function, no
threads — fast and deterministic.

### Edge cases (baked into the design)
- `sysconf` returns -1 -> floor to 1 (never spawn 0 or negative threads).
- `GOMAXPROCS=0` / negative / non-numeric -> NCPU fallback.
- Deadlock detector timing gate (3 x 0.5ms) is wall-clock based and N-independent — no
  change needed.

### Local gate before merge
`make verify` all-green at pinned `GOMAXPROCS=4` + `make test` 76/1 + both soak probes
green across their full 50-iteration loops.

## 5. Wiring

- `Makefile`: add `parallel-soak-probe` and `parallel-select-soak-probe` targets; append
  both to the `verify:` dependency list (after `deadlock-goroutine-probe`).
- `.github/workflows/tests.yml`: append both targets to the make-target list; set
  `GOMAXPROCS=4` in the job environment for reproducibility.
- New example programs: `examples/parallel_soak_probe.goo`,
  `examples/parallel_select_soak_probe.goo`.

## 6. Success criteria
- Default lazy-init spawns NCPU (or `GOMAXPROCS`) workers, clamped to `[1, 16]`.
- `GOMAXPROCS` override honored; invalid values fall back to NCPU.
- Full `verify:` suite green under `GOMAXPROCS=4`; `make test` 76/1.
- Both soak probes green across all 50 iterations (no wrong output, no nonzero exit, no
  false deadlock abort).
- Explicit-init C stress tests unaffected.
- No change to `goo_scheduler_init` signature or the deadlock detector.
