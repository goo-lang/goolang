# M8d — Pre-Default-Flip Concurrency Prerequisites

**Status:** Design (approved 2026-06-28)
**Milestone:** M8d (concurrency runtime hardening — debt before multi-core default)
**Predecessor:** M8c — correct M:N scheduler (PR #32 merged, `main` @ `24424b1`). M8c's final review logged three §8a prerequisites that must hold before `num_threads` default is flipped to N>1.
**Theme:** Pay down the three §8a items as hardening/hygiene. **The default thread count is NOT flipped here** — that remains a separate later change once these prove out.

---

## 1. Summary

M8c made the M:N scheduler correct for N>1 but kept the lazy default at 1, and its final review surfaced three items to address before any default-flip. M8d lands all three:

1. **Channel-block bookkeeping hygiene** — `channels.c` reads `g_scheduler->current_goroutine`, which M8c leaves permanently NULL, so the bookkeeping is dead. Route it through a `goo_current_goroutine()` accessor over the per-thread `t_current` so it is correct again. (Channel-deadlock *detection* itself remains unimplemented — see §6 — and is out of scope.)
2. **Close the `goo_yield` save-before-publish race** — `goo_yield` re-enqueues a goroutine before `swapcontext` saves its context; at N>1 another worker could run it first. Fix via a scheduler-side handoff, mirroring M8c's `t_reap` reap pattern.
3. **Channel-under-N=4 stress coverage** — M8c's stress test exercises exit/reap but not channels. Add a test driving channel send/recv across 4 workers.

This is hardening: none of it changes what shipping (single-worker) programs do.

---

## 2. Scope decisions (locked)

1. **Default stays 1.** No change to `goo_go`'s lazy `goo_scheduler_init(1)`. Flipping the default is explicitly out of scope (a later milestone).
2. **Item 1 is hygiene only.** Make the channel-wait bookkeeping per-thread-correct; do NOT attempt to make the deadlock detector see channel-blocked goroutines (that needs a blocked-set the detector can traverse — a separate, larger milestone). No "detector fixed" claim anywhere.
3. **Item 2 uses the existing handoff pattern.** Mirror `t_reap`: a new `_Thread_local t_requeue` set by `goo_yield`, acted on by the scheduler after the swap. Do not invent a new synchronization mechanism.
4. **Runtime-only** except the one accessor declaration. The accessor (`goo_current_goroutine`) is added to `include/runtime.h` → requires `make clean && make goo`.

---

## 3. Item 1 — channel-block bookkeeping via accessor

**Problem.** `src/runtime/channels.c` (lines ~241-255 send-wait, ~358-372 recv-wait) sets `waiting_on_channel` / `waiting_for_send` / `state = BLOCKED` through `g_scheduler->current_goroutine`. M8c moved "current goroutine" to the per-thread `t_current` (file-static in `concurrency.c`) and no longer writes `g_scheduler->current_goroutine`, so it stays NULL from init — every `if (g_scheduler && g_scheduler->current_goroutine)` guard is now always false and the bookkeeping is dead code.

**Fix.**
- Add to `src/runtime/concurrency.c`: `goo_goroutine_t* goo_current_goroutine(void) { return t_current; }`.
- Declare in `include/runtime.h` near the other scheduler API: `goo_goroutine_t* goo_current_goroutine(void);`.
- In `src/runtime/channels.c`, replace each `g_scheduler->current_goroutine` read with a local `goo_goroutine_t* self = goo_current_goroutine();` and guard on `self` (preserving the existing field writes). The bookkeeping then reflects the actual running goroutine on each worker.

**Explicitly out of scope:** making `goo_deadlock_check` observe channel-blocked goroutines. The detector traverses `g_scheduler->ready_queue` (`deadlock.c:132,157,184`), but a channel-blocked goroutine is parked in `goo_chan_send/recv`'s `pthread_cond_wait`, not in `ready_queue`. Restoring the bookkeeping does NOT make the detector see it. A working channel-deadlock detector needs a separate blocked-goroutine set the detector can walk — deferred to its own milestone. This limitation is documented in a code comment at the channels.c bookkeeping sites and here.

---

## 4. Item 2 — close the `goo_yield` race (scheduler-side handoff)

**Problem.** `goo_yield` (`concurrency.c:186-205`) sets `state = READY`, calls `scheduler_add_goroutine(current)` (making it visible in the ready queue), then `swapcontext(&current->context, &t_sched_ctx)`. At N>1, another worker can dequeue and `swapcontext` *into* `current->context` before this worker's `swapcontext` finishes writing it — a race on `current->context`. (Pre-existing; currently unreachable because nothing calls `goo_yield` — channels use `cond_wait`.)

**Fix (mirror `t_reap`).**
- Add `static _Thread_local goo_goroutine_t* t_requeue = NULL;` in `concurrency.c`.
- `goo_yield`: set `state = READY`, set `t_requeue = current`, and `swapcontext(&current->context, &t_sched_ctx)` — **do not** call `scheduler_add_goroutine` here.
- `scheduler_main_loop`: set `t_requeue = NULL` before each swap (next to the existing `t_reap = NULL`); after the swap returns, in addition to the `t_reap` reap, add: `if (t_requeue) { scheduler_add_goroutine(t_requeue); t_requeue = NULL; }`.
- Result: the yielded goroutine becomes visible in the ready queue only *after* `swapcontext` has fully saved its context onto the worker's stack → no other worker can swap into a half-saved context. `t_reap` (exit) and `t_requeue` (yield) are mutually exclusive per run.

---

## 5. Item 3 — channel-under-N=4 stress test

A C test linking `lib/libgoo_runtime.a`: `goo_scheduler_init(4)`; spawn P producer goroutines, each `goo_chan_send`ing a known value into a shared buffered channel (`goo_make_chan(sizeof(int), cap)`); `main` `goo_chan_recv`s P values and checks their sum; loop ≥50 batches under `timeout`. Exercises channel send/recv under true 4-worker parallelism. Expected GREEN on current code (a regression guard); if it surfaces a latent channel-MT bug it becomes RED and is fixed under systematic-debugging before this milestone closes.

Also add a **yield-stress** test (Item 2's RED→GREEN): `goo_scheduler_init(4)`; spawn goroutines that call `goo_yield` in a loop then deliver a value; check the aggregate; loop. RED before Item 2's fix (context race), GREEN after.

---

## 6. Testing / CI gates

- New C stress targets `chan-mt-stress` and `yield-stress`, plus an accessor unit check (a goroutine calls `goo_current_goroutine()` and verifies it returns its own non-NULL handle), each run under `timeout`, wired into BOTH `verify:` and `.github/workflows/tests.yml`.
- `yield-stress` must be RED before Item 2 and GREEN after.
- Full pre-existing suite + `make test` (76 passed / 1 skipped) stay green — default is still 1 worker.
- Header change (the accessor) ⇒ `make clean && make goo`.
- Verified **locally** (CI on `dd0wney` may be billing-blocked).

---

## 7. Out of scope (later milestones)

- **Flipping the default thread count** to NCPU / a fixed N (the payoff this milestone de-risks).
- **Working channel-deadlock detection** (a blocked-goroutine set the detector traverses).
- Condvar-based channel parking refinements, work-stealing, OpenMP `parallel_for` / `WaitGroup`, `defer`/`recover`.

---

## 8. Success criteria

M8d is complete when:
1. `channels.c` no longer reads `g_scheduler->current_goroutine`; it uses `goo_current_goroutine()` and the bookkeeping reflects the per-worker running goroutine.
2. `goo_yield` no longer re-enqueues before the swap; the scheduler re-enqueues via `t_requeue` after the swap. `yield-stress` is RED before and GREEN after.
3. `chan-mt-stress` passes under N=4 (≥50 batches, 0 crashes/hangs, correct aggregate).
4. The channel-deadlock-detection limitation is documented (code comment + spec); no "detector fixed" claim is made.
5. Default unchanged (`num_threads=1`); full pre-existing suite + `make test` stay green.
6. New tests wired into `verify:` + `tests.yml`.
7. Verified locally (CI may be billing-blocked).
