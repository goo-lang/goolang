# M8c — Correct M:N Goroutine Scheduler (deferred stack-free + per-thread context)

**Status:** Design (approved 2026-06-28)
**Milestone:** M8c (concurrency runtime hardening)
**Predecessor:** M8/M8b — goroutines, unbuffered channels, `select`, goroutine-arg escape safety (PRs #29, #30 merged to `main` @ `2734fef`).
**Theme:** Make the existing multi-threaded (M:N) scheduler *correct* so goroutines can run on multiple OS threads without crashing — the prerequisite for real parallelism (OpenMP-style `parallel_for`, `WaitGroup`, later milestones).

---

## 1. Summary

Goo's scheduler is already M:N: `goo_scheduler_init(N)` spawns N OS threads (`scheduler_main_loop`), each pulling green (ucontext) goroutines from a shared, mutex-protected ready queue (`src/runtime/concurrency.c`). The queue and channel operations are already thread-safe. But three defects in **per-thread scheduler state** make N>1 crash (~33% of runs at N=4; see §2), so the runtime is pinned to `num_threads=1`. M8c fixes those three defects so the scheduler is correct for N>1, leaving the default at 1 (a later milestone flips it). Runtime-only change; no grammar, codegen, or channel-semantics change.

---

## 2. Root causes (confirmed; reproduced)

Reproduction: a C driver calling `goo_scheduler_init(4)` + 32 goroutines + `goo_scheduler_wait()`, run 30×, gave **OK=17, CRASH=10, HANG=3** — matching the historical 2→6/30, 3→21/30 signature.

1. **Use-after-free of the running stack** (`concurrency.c:191-216`). `goroutine_wrapper` runs on the goroutine's own ucontext stack and calls `goo_goroutine_exit`, which does `goo_free(current->stack)` (line 204) and then keeps executing on that freed memory through `goo_free(current)`, the `current_goroutine = NULL` write, and `setcontext` (line 213). Single-threaded, nothing reallocates the freed block before `setcontext` switches away, so it is masked; with ≥2 OS threads another thread's allocator can hand out that block mid-exit → corruption.
2. **Shared `main_context`** (`concurrency.c`). `g_scheduler->main_context` is a single field, but all N worker threads `swapcontext` into it and `uc_link` (`:157`) points to it — N threads stomping one return context.
3. **Shared `current_goroutine`** (`concurrency.c`). A single field written/read by all N workers (set at `:236`, read in exit at `:196/:207`) — two workers running two goroutines corrupt each other's notion of "current".

The ready queue (`scheduler_get_next_goroutine`/`scheduler_add_goroutine`, `:270-290`) is already mutex-locked and is **not** a defect.

---

## 3. Locked scope decisions

1. **Keep the existing M:N green-thread model.** Fix the three defects; do not rewrite to 1:1 pthread-per-goroutine. Rejected 1:1: discards working green-thread infra and caps goroutine counts at OS-thread limits. Rejected "UAF-only / stay single-threaded": does not deliver multi-core correctness.
2. **Default thread count stays 1.** `goo_go`'s lazy `goo_scheduler_init(1)` is unchanged, so every existing program/probe remains single-worker and deterministic. The scheduler becomes correct for N>1; flipping the global default to multi-core is a deliberate, separate later change. Rejected flipping to NCPU/2 now: would activate true parallelism across all existing probes in one step, surfacing any latent channel/select/deadlock-detector races in the same change.
3. **Runtime-only.** No grammar, codegen, channel-semantics, or public-API change. `goo_scheduler_init(N)` is the existing opt-in used by the stress test.
4. **Per-thread state via `_Thread_local`** (C11/C23 thread-local storage), not a per-thread struct array.

---

## 4. Architecture / the fix (all in `src/runtime/concurrency.c`)

### 4.1 Per-thread scheduler state
Replace the shared `g_scheduler->main_context` and `g_scheduler->current_goroutine` with thread-locals owned by each worker thread:

```c
static _Thread_local ucontext_t       t_sched_ctx;  // this worker's scheduler-return context
static _Thread_local goo_goroutine_t* t_current;    // goroutine currently running on this worker
static _Thread_local goo_goroutine_t* t_reap;       // goroutine to free after swap (set on EXIT only)
```

(Use the actual context type used by the goroutine `context` field — `ucontext_t`. The `goo_scheduler_t` struct's `main_context`/`current_goroutine` fields are left in place to avoid a header change. `main_context` becomes genuinely unused. **`current_goroutine` is NOT unused:** `src/runtime/channels.c` still reads/writes it for channel-wait bookkeeping (marking a goroutine `BLOCKED` / `waiting_on_channel`). After this change the scheduler no longer writes it, so it stays NULL and that bookkeeping becomes inert — a **known, tracked regression** of the best-effort deadlock detector, see §8a. It is non-fatal: channel block/wake is condvar-based and independent of this field, and the detector (which walks `ready_queue`) could not observe cond_wait-blocked goroutines even before this change. ucontext keeps the same OS thread across a switch, so a goroutine and the worker running it share these thread-locals.)

### 4.2 Deferred stack-free (reap from the scheduler, via a thread-local)
- `goo_goroutine_exit`: set `state = GOO_GOROUTINE_DONE`, decrement `stats.num_goroutines` under the mutex, set `t_reap = current`, then `setcontext(&t_sched_ctx)`. **Remove** `goo_free(current->stack)` and `goo_free(current)`.
- `scheduler_main_loop`: set `t_reap = NULL` before each `swapcontext(&t_sched_ctx, &g->context)`; immediately after it returns:
  ```c
  if (t_reap) {                 // set iff the goroutine EXITED (not yielded)
      goo_free(t_reap->stack);
      goo_free(t_reap);
      t_reap = NULL;
  }
  t_current = NULL;
  ```
  Safe: the scheduler runs on the worker's own stack, never the goroutine's, and it never dereferences the goroutine after the swap.

### 4.3 Race-free ownership (why reaping via `t_reap` is correct)
- The worker must **not** read `g->state` (or any `g->*`) after the swap returns: a **yielded** goroutine (`goo_yield`, `:174-178`) sets `state = READY` and re-enqueues itself *before* swapping, so by the time the worker resumes, another worker may have already dequeued, run, finished, and freed it — reading `g->state` would itself be a use-after-free.
- The disposition is therefore carried by `t_reap` (a thread-local set on the same OS thread before the switch back): **exit** sets `t_reap = current`; **yield** leaves it `NULL`.
- A goroutine that set `t_reap` is **DONE** and is never re-enqueued, so the running worker holds the sole reference and frees it safely. A yielded goroutine leaves `t_reap` NULL, so the worker never touches it. The two paths are disjoint → race-free.

### 4.4 Per-running-thread `uc_link` and yield
- Before each `swapcontext` in the loop, set `g->context.uc_link = &t_sched_ctx` so a goroutine that returns normally (backstop) goes to the worker that ran it.
- `goo_yield`: swap `&current->context` with the thread-local `&t_sched_ctx` (using `t_current`), not the shared `main_context`.

---

## 5. Components touched

| Unit | File | Change |
|---|---|---|
| Worker thread-local state | `src/runtime/concurrency.c` | add `_Thread_local t_sched_ctx`, `t_current`, `t_reap`; stop using `g_scheduler->{main_context,current_goroutine}` in the swap/exit/reap logic |
| `goo_goroutine_exit` | `src/runtime/concurrency.c:191` | mark DONE + dec stats + set `t_reap` + `setcontext(&t_sched_ctx)`; no self-free |
| `scheduler_main_loop` | `src/runtime/concurrency.c:228` | set `uc_link`/`t_current`/`t_reap=NULL` per iteration; reap via `t_reap` after `swapcontext` |
| `goo_yield` | `src/runtime/concurrency.c:169` | use thread-local context |
| struct fields | `include/runtime.h` / `goo_scheduler_t` | **left in place, header untouched.** `main_context` becomes dead; `current_goroutine` is still referenced by `channels.c` (now always NULL) — see §8a follow-up. No `make clean` needed. |

No new files. No public API change. No header change.

---

## 6. Testing / CI gates

- **`mt-scheduler-stress` (RED→GREEN acceptance):** a C test linking `lib/libgoo_runtime.a` that calls `goo_scheduler_init(4)`, spawns goroutines in batches (each does bounded work and increments a shared C11 `atomic_int`), `goo_scheduler_wait()`, and checks the per-batch count. Run as an **internal loop (≥50 batches)** under `timeout`. Must FAIL before the fix (crash/hang/wrong-count in a meaningful fraction) and PASS after (0 crashes, 0 hangs, correct count, every batch). A `.goo` probe cannot request N threads, so this acceptance test is C-level by necessity — stated explicitly. NOTE: it deliberately does **not** use channels or `goo_yield`; it targets the exit/reap/per-thread-context defects that were the actual bug. Channel-under-N=4 coverage is a tracked follow-up (§8a).
- Added as a Makefile target and wired into BOTH `verify:` and `.github/workflows/tests.yml`.
- **Regression:** the default is unchanged (1 worker), so the full pre-existing probe suite + `make test` (76 passed / 1 skipped) must stay green — the guard that single-threaded behavior is untouched.
- `make test` (unit suite) shows no new failures.
- Verified **locally** (CI on `dd0wney` may be billing-blocked).

---

## 7. Out of scope (later milestones)

- Flipping the default thread count to NCPU (separate change once confidence is high).
- Condvar-based channel parking (channels still busy-yield), work-stealing scheduler.
- OpenMP-style `parallel_for` stdlib + `WaitGroup` binding.
- `defer`/`recover`, Erlang-style supervision.
- A `.goo`-level way to request N worker threads.

---

## 8. Known risks

- **ucontext + threads is subtle.** Each worker must only ever `swap`/`set` its own `t_sched_ctx`; the design keeps all context state thread-local to enforce this.
- **Latent races elsewhere stay dormant** because the default is single-worker; they will surface when the default is flipped (tracked, §8a). The stress test exercises the **exit/reap/per-thread-context** paths under true N=4 parallelism — exactly what was broken. It does **not** exercise channels or `goo_yield`; channel-under-true-parallelism coverage is a tracked follow-up (§8a).

## 8a. Tracked pre-default-flip follow-up (must all hold before N>1 becomes the default)

These were surfaced by the final whole-branch review. None block M8c (default stays 1) but all gate the eventual default-flip:

1. **Deadlock-detector / `current_goroutine` regression.** `channels.c` marks channel-blocked goroutines via `g_scheduler->current_goroutine`, which M8c leaves NULL. Restore detector visibility (e.g. a `goo_current_goroutine()` accessor returning `t_current`, or sync the field, or delete the dead bookkeeping and document). Non-fatal today (condvar-based block/wake; detector already ineffective for channel deadlocks).
2. **`goo_yield` save-before-publish race.** `goo_yield` re-enqueues `current` before `swapcontext` writes `current->context`; at N>1 another worker could swap into it first. Currently **unreachable** (no caller — channels use cond_wait). Must be fixed before any `goo_yield` caller is added *and* before the default-flip (swap-then-handoff).
3. **Channel-using N=4 stress variant.** Add a stress test that drives channel send/recv across workers under N=4 (the current test does not).

---

## 9. Success criteria

M8c is complete when:
1. `mt-scheduler-stress` is RED before the fix and GREEN after — ≥50 iterations at N=4 with 0 crashes, 0 hangs, correct aggregated result.
2. `goo_goroutine_exit` no longer frees the stack it runs on; the scheduler reaps DONE goroutines.
3. The scheduler's swap/exit/reap logic uses only per-worker thread-local context (`t_sched_ctx`/`t_current`/`t_reap`); the scheduler no longer writes the shared `main_context`/`current_goroutine` fields. (The `current_goroutine` field is still read by `channels.c` — now inert; restoring that is the §8a follow-up.)
4. Default behavior unchanged (`num_threads=1` lazy init); full pre-existing probe suite + `make test` stay green.
5. Wired into `verify:` + `tests.yml`.
6. Verified locally (CI may be billing-blocked).
