# M8c — Correct M:N Goroutine Scheduler Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the existing M:N green-thread scheduler correct under multiple OS threads by fixing the running-stack use-after-free and the shared per-thread scheduler state, so goroutines can run on N>1 workers without crashing.

**Architecture:** Runtime-only change in `src/runtime/concurrency.c`. Move `main_context` and `current_goroutine` from the shared `g_scheduler` struct to `_Thread_local` worker state; make a dying goroutine mark itself DONE and return to its worker's scheduler context without freeing its own stack; have the worker reap the DONE goroutine after `swapcontext` returns. Default thread count stays 1; correctness for N>1 is proven by a C stress test that calls `goo_scheduler_init(4)`.

**Tech Stack:** C23, ucontext (`<ucontext.h>`), pthreads, C11 `_Thread_local` + `<stdatomic.h>`, GNU Make.

## Global Constraints

- **Keep the M:N model.** Fix the three defects; do not rewrite to 1:1 pthread-per-goroutine. (Spec §3.1)
- **Default thread count stays 1.** Do NOT change `goo_go`'s lazy `goo_scheduler_init(1)`. The scheduler becomes correct for N>1; flipping the default is a separate later change. (Spec §3.2)
- **Runtime-only.** No grammar, codegen, channel-semantics, or public-API change. No new header symbols. (Spec §3.3)
- **No header edit.** Leave the `current_goroutine`/`main_context` fields in `struct goo_scheduler` (`include/runtime.h:384-391`) in place to avoid a `make clean`. The scheduler's swap/exit/reap logic stops reading/writing them (the `_Thread_local` worker state supersedes them). NOTE: `main_context` becomes dead, but `current_goroutine` is **still referenced by `src/runtime/channels.c`** for deadlock-detector bookkeeping — after this change it stays NULL, making that bookkeeping inert (a tracked, non-fatal follow-up; channel block/wake is condvar-based and independent). Do not assume it is fully unused. (Spec §4.1, §5, §8a)
- **Reap rule is DONE-only.** The scheduler frees a goroutine's stack+struct only when its state is `GOO_GOROUTINE_DONE`. A yielded goroutine re-enqueues itself as `READY` and must never be reaped by the running worker. (Spec §4.3)
- **Build facts:** compiler `bin/goo`, runtime `lib/libgoo_runtime.a`. Build: `make goo lib/libgoo_runtime.a`. No `include/*.h` edits in this plan, so `make clean` is not required. `make verify` halts at `ccomp-build` (CompCert gap) — ignore; the real gate is the CI probe list + `make test`.
- **Run all concurrency tests under a timeout** so a scheduler hang fails loudly.
- **Shell is zsh:** an unquoted `$VAR` does NOT word-split; pass make target lists literally (not via a variable).
- **CI billing caveat:** Actions on `dd0wney/goolang` may be billing-blocked (jobs show red, never start). Authoritative verification is LOCAL: the probe list + `make test`.
- **clang/LSP false positives:** "header not found" / "unknown type" diagnostics are not real (build uses `-Iinclude`). Trust `make`.

## Reference map

| Concern | Location | Notes |
|---|---|---|
| `goo_yield` (uses shared ctx) | `src/runtime/concurrency.c:169-189` | sets `current->state=READY`, self re-enqueues, `swapcontext(&current->context, &g_scheduler->main_context)` |
| `goo_goroutine_exit` (UAF) | `src/runtime/concurrency.c:191-216` | line 204 `goo_free(current->stack)`, 205 `goo_free(current)`, 213 `setcontext(&g_scheduler->main_context)` |
| `goroutine_wrapper` | `src/runtime/concurrency.c:218-225` | reads `g_scheduler->current_goroutine` |
| `scheduler_main_loop` | `src/runtime/concurrency.c:228-268` | line 236 sets `g_scheduler->current_goroutine`, 246 `swapcontext(&g_scheduler->main_context, &goroutine->context)` |
| ready queue (already locked — DO NOT change) | `src/runtime/concurrency.c:270-290` | `scheduler_get_next_goroutine` / `scheduler_add_goroutine` |
| scheduler struct (leave as-is) | `include/runtime.h:384-399` | `current_goroutine` (386), `main_context` (390) — leave unused |
| goroutine context field | `include/runtime.h:303` | `ucontext_t context;` |
| state enum | `include/runtime.h:231-234` | `GOO_GOROUTINE_{READY,RUNNING,BLOCKED,DONE}` |
| public API used by the test | `include/runtime.h` | `void goo_scheduler_init(int)`, `goo_goroutine_t* goo_go(goo_goroutine_func_t, void*)`, `void goo_scheduler_wait(void)`; `goo_goroutine_func_t = void(*)(void*)` |
| C-test link pattern | (reproduction) | `$(CC) -std=c23 -D_GNU_SOURCE -Iinclude -I. test.c lib/libgoo_runtime.a -lpthread -lm` |
| Makefile probe pattern to mirror | `Makefile` `go-probe:` / `verify:` | probe target shape + dependency list |
| CI probe list | `.github/workflows/tests.yml:54` | space-separated make targets |

---

## Task 1: Failing `mt-scheduler-stress` C test (RED)

**Files:**
- Create: `tests/concurrency/mt_scheduler_stress.c`

**Interfaces:**
- Consumes: runtime `goo_scheduler_init(int)`, `goo_go(goo_goroutine_func_t, void*)`, `goo_scheduler_wait(void)` (all in `include/runtime.h`).
- Produces: the binary `build/mt_scheduler_stress` (built in Task 3's target); for Task 1 it is compiled ad hoc to confirm RED.

- [ ] **Step 1: Write the stress test**

Create `tests/concurrency/mt_scheduler_stress.c`:

```c
// mt-scheduler-stress: exercises the M:N scheduler under 4 OS threads.
// RED before M8c (UAF on the goroutine's own stack + shared main_context/
// current_goroutine across workers) → crashes/corrupts in a meaningful
// fraction of the 100 batches. GREEN after M8c → 0 crashes, correct count.
#define _GNU_SOURCE
#include "runtime.h"
#include <stdatomic.h>
#include <stdio.h>

static atomic_int g_counter;

static void worker(void* arg) {
    (void)arg;
    volatile long x = 0;
    for (int i = 0; i < 2000; i++) x += i;   // bounded work, forces real interleaving
    atomic_fetch_add(&g_counter, 1);
}

int main(void) {
    goo_scheduler_init(4);                    // force real multi-threading (default is 1)
    const int BATCHES = 100;
    const int PER = 16;
    for (int b = 0; b < BATCHES; b++) {
        atomic_store(&g_counter, 0);
        for (int i = 0; i < PER; i++) {
            goo_go(worker, NULL);
        }
        goo_scheduler_wait();                 // block until this batch's goroutines finish
        int got = atomic_load(&g_counter);
        if (got != PER) {
            fprintf(stderr, "batch %d: counter=%d expected %d\n", b, got, PER);
            return 1;
        }
    }
    printf("mt-scheduler-stress OK (%d batches x %d goroutines @ 4 threads)\n", BATCHES, PER);
    return 0;
}
```

- [ ] **Step 2: Build the runtime and compile the test**

Run:
```bash
make goo lib/libgoo_runtime.a
mkdir -p build
gcc -std=c23 -D_GNU_SOURCE -Iinclude -I. tests/concurrency/mt_scheduler_stress.c lib/libgoo_runtime.a -lpthread -lm -o build/mt_scheduler_stress
```
Expected: compiles cleanly. (Use `gcc-14` if the default `gcc` is unavailable; the project CI uses `CC=gcc-14`.)

- [ ] **Step 3: Run it several times to confirm it FAILS (RED)**

Run:
```bash
ok=0; bad=0; for i in $(seq 1 20); do timeout 30 ./build/mt_scheduler_stress >/dev/null 2>&1 && ok=$((ok+1)) || bad=$((bad+1)); done; echo "OK=$ok BAD(crash/hang/wrong)=$bad / 20"
```
Expected: **BAD > 0** (crashes, hangs at 124, or non-zero from a wrong count) in a meaningful fraction — the pre-fix scheduler is broken at N=4. Capture the numbers as RED. (If by chance 20/20 pass, raise BATCHES to 400 and re-run — the race is probabilistic but reliably appears across enough exit/reap cycles.)

- [ ] **Step 4: Commit the failing test**

```bash
git add tests/concurrency/mt_scheduler_stress.c
git commit --no-gpg-sign -m "test(runtime): add mt-scheduler-stress (RED) — M:N scheduler crashes at num_threads=4"
```

---

## Task 2: Fix the scheduler — per-thread context + deferred reap (GREEN)

**Files:**
- Modify: `src/runtime/concurrency.c` (`goo_yield`, `goo_goroutine_exit`, `goroutine_wrapper`, `scheduler_main_loop`; add `_Thread_local` worker state)

**Interfaces:**
- Consumes: `ucontext_t` (`<ucontext.h>`, already included at `concurrency.c:14`), `GOO_GOROUTINE_{READY,RUNNING,DONE}`, the locked `scheduler_add_goroutine`/`scheduler_get_next_goroutine`.
- Produces: a scheduler correct for N>1. No signature changes.

- [ ] **Step 1: Add `_Thread_local` worker state**

Near the top of `src/runtime/concurrency.c` (after the includes and the `g_scheduler` global definition), add:

```c
// M8c: per-worker scheduler state. Each OS worker thread has its own scheduler
// return context and "currently running goroutine". These supersede the shared
// g_scheduler->main_context / ->current_goroutine fields (which are left unused
// to avoid a header change). Sharing those single fields across N workers was
// the multi-thread corruption (stomped return context / wrong current).
//
// t_reap carries the disposition decision back to the worker WITHOUT
// dereferencing the goroutine after the swap: a goroutine that EXITS sets
// t_reap to itself (it is never re-enqueued, so only this worker references it,
// so this worker frees it); a goroutine that YIELDS leaves t_reap NULL (it
// re-enqueued itself and may already be running/freed on another worker, so
// this worker must not touch it). ucontext keeps the same OS thread across the
// switch, so the goroutine and its worker share these thread-locals.
static _Thread_local ucontext_t       t_sched_ctx;
static _Thread_local goo_goroutine_t* t_current = NULL;
static _Thread_local goo_goroutine_t* t_reap = NULL;
```

- [ ] **Step 2: Rewrite `goo_goroutine_exit` — no self-free, return to this worker's scheduler**

Replace the body of `goo_goroutine_exit` (`src/runtime/concurrency.c:191-216`) with:

```c
void goo_goroutine_exit(void) {
    goo_goroutine_t* current = t_current;
    if (!g_scheduler || !current) {
        return;
    }

    current->state = GOO_GOROUTINE_DONE;

    goo_mutex_lock(g_scheduler->scheduler_mutex);
    g_scheduler->stats.num_goroutines--;
    goo_mutex_unlock(g_scheduler->scheduler_mutex);

    // Hand this goroutine to the worker for reaping. Do NOT free current->stack
    // or current here: this code is executing ON current->stack. Setting t_reap
    // (this OS thread's thread-local) tells the worker — after swapcontext
    // returns onto its own stack — to free us. A yielding goroutine leaves
    // t_reap NULL, so it is never freed from under another worker.
    t_reap = current;

    // Return control to this worker's scheduler context.
#ifdef GOO_PLATFORM_UNIX
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    setcontext(&t_sched_ctx);
#pragma clang diagnostic pop
#endif
}
```

- [ ] **Step 3: Point `goroutine_wrapper` at the thread-local current**

In `goroutine_wrapper` (`src/runtime/concurrency.c:218-225`), replace `goo_goroutine_t* current = g_scheduler->current_goroutine;` with the thread-local:

```c
static void goroutine_wrapper(void) {
    goo_goroutine_t* current = t_current;
    if (current && current->function) {
        current->function(current->arg);
    }
    goo_goroutine_exit();
}
```

- [ ] **Step 4: Rewrite the `scheduler_main_loop` run+reap block**

In `scheduler_main_loop` (`src/runtime/concurrency.c:228-268`), replace the `if (goroutine) { ... }` block that currently sets `g_scheduler->current_goroutine` and does `swapcontext(&g_scheduler->main_context, &goroutine->context)` with the thread-local version that sets `uc_link`, runs, then reaps DONE goroutines:

```c
        if (goroutine) {
            t_current = goroutine;
            t_reap = NULL;                 // cleared each run; the goroutine sets it on exit
            goroutine->state = GOO_GOROUTINE_RUNNING;

            goo_mutex_lock(g_scheduler->scheduler_mutex);
            g_scheduler->stats.context_switches++;
            goo_mutex_unlock(g_scheduler->scheduler_mutex);

#ifdef GOO_PLATFORM_UNIX
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            // Return to THIS worker's scheduler context when the goroutine
            // exits or yields.
            goroutine->context.uc_link = &t_sched_ctx;
            if (swapcontext(&t_sched_ctx, &goroutine->context) == -1) {
                goo_panic("Failed to switch to goroutine");
            }
#pragma clang diagnostic pop
#endif

            // Back on this worker's own stack. Reap ONLY via t_reap, which the
            // goroutine set (to itself) iff it EXITED. Do NOT read goroutine->*
            // here: if it yielded, it re-enqueued itself and another worker may
            // already have run and freed it — dereferencing it would be a UAF.
            // A DONE goroutine is never re-enqueued, so t_reap is ours alone.
            if (t_reap) {
                goo_free(t_reap->stack);
                goo_free(t_reap);
                t_reap = NULL;
            }
            t_current = NULL;
        } else {
```

(Leave the `else { deadlock check / sleep }` branch and the trailing `stats.scheduler_cycles` bookkeeping unchanged.)

- [ ] **Step 5: Point `goo_yield` at the thread-local context**

Replace the body of `goo_yield` (`src/runtime/concurrency.c:169-189`) with:

```c
void goo_yield(void) {
    goo_goroutine_t* current = t_current;
    if (!g_scheduler || !current) {
        return;
    }

    current->state = GOO_GOROUTINE_READY;
    scheduler_add_goroutine(current);   // re-enqueue self before switching away

#ifdef GOO_PLATFORM_UNIX
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (swapcontext(&current->context, &t_sched_ctx) == -1) {
        goo_panic("Failed to yield goroutine");
    }
#pragma clang diagnostic pop
#endif
}
```

- [ ] **Step 6: Build and confirm the stress test now PASSES (GREEN)**

Run:
```bash
make goo lib/libgoo_runtime.a
gcc -std=c23 -D_GNU_SOURCE -Iinclude -I. tests/concurrency/mt_scheduler_stress.c lib/libgoo_runtime.a -lpthread -lm -o build/mt_scheduler_stress
ok=0; bad=0; for i in $(seq 1 20); do timeout 30 ./build/mt_scheduler_stress >/dev/null 2>&1 && ok=$((ok+1)) || bad=$((bad+1)); done; echo "OK=$ok BAD=$bad / 20"
```
Expected: **OK=20 BAD=0** — every run completes with the correct count, no crash/hang. (If any BAD remains, STOP — the fix is incomplete; do not paper over it. Re-check that all four edits use `t_sched_ctx`/`t_current` and that no path still reads `g_scheduler->main_context`/`->current_goroutine`.)

- [ ] **Step 7: Confirm no shared-context references remain**

Run:
```bash
grep -n "g_scheduler->main_context\|g_scheduler->current_goroutine" src/runtime/concurrency.c || echo "CLEAN: no shared scheduler-context references"
```
Expected: `CLEAN` (all uses moved to thread-locals).

- [ ] **Step 8: Commit**

```bash
git add src/runtime/concurrency.c
git commit --no-gpg-sign -m "fix(runtime): per-thread scheduler context + deferred stack-free (M:N correct at N>1)"
```

---

## Task 3: Wire the gate + full regression

**Files:**
- Modify: `Makefile` (add `mt-scheduler-stress:` target; append to `verify:`)
- Modify: `.github/workflows/tests.yml:54` (append `mt-scheduler-stress`)

**Interfaces:**
- Consumes: `tests/concurrency/mt_scheduler_stress.c`, `$(RUNTIME_LIB)`, `$(CC)`.
- Produces: CI-gated `mt-scheduler-stress`.

- [ ] **Step 1: Add the Makefile target**

In `Makefile`, after the existing `go-probe:` target, add (TAB-indented, matching the file's recipe style):

```make
# M8c: M:N scheduler correctness under real multi-threading (num_threads=4).
mt-scheduler-stress: $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== mt-scheduler-stress: M:N scheduler correct under num_threads=4 ==="
	$(CC) -std=c23 -D_GNU_SOURCE -Iinclude -I. tests/concurrency/mt_scheduler_stress.c $(RUNTIME_LIB) -lpthread -lm -o build/mt_scheduler_stress
	@timeout 60 ./build/mt_scheduler_stress; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "mt-scheduler-stress: PASS"; else echo "mt-scheduler-stress: FAIL (exit $$rc — crash/hang/wrong count)"; exit 1; fi
```

- [ ] **Step 2: Append to the `verify:` target**

In `Makefile`, find the `verify:` dependency list and append ` mt-scheduler-stress` to its end.

- [ ] **Step 3: Append to the CI probe list**

In `.github/workflows/tests.yml:54`, append ` mt-scheduler-stress` to the end of the space-separated make-target list.

- [ ] **Step 4: Run the new target and the full regression gate**

Run (literal target list — zsh does not word-split a variable):
```bash
make mt-scheduler-stress
make go-probe chan-probe chan-elem-probe chan-padded-probe chan-uint-probe unbuffered-probe select-probe block-scope-probe escape-probe escape-range-probe 2>&1 | grep -E "PASS|FAIL"
make test 2>&1 | grep -E "Tests run|Passed:|Skipped:|Failed|All tests"
```
Expected: `mt-scheduler-stress: PASS`; every regression probe `PASS`; `make test` 76 passed / 1 skipped (baseline). The default is still 1 worker, so single-threaded behavior must be unchanged.

- [ ] **Step 5: Commit**

```bash
git add Makefile .github/workflows/tests.yml
git commit --no-gpg-sign -m "ci(runtime): gate mt-scheduler-stress in verify: and tests.yml"
```

---

## Final verification

- [ ] `mt-scheduler-stress` was RED before Task 2 (BAD>0 / 20) and is GREEN after (20/20, exit 0) under timeout.
- [ ] `goo_goroutine_exit` no longer frees the running stack; the scheduler reaps DONE goroutines after `swapcontext`.
- [ ] No `g_scheduler->main_context` / `->current_goroutine` references remain (Step 7 grep CLEAN).
- [ ] Default unchanged (`num_threads=1` lazy init); full probe suite + `make test` (76/1) green.
- [ ] Wired into `verify:` + `tests.yml`.
- [ ] Spec §9 success criteria met.

## Spec coverage self-check

| Spec §9 / §4 element | Task |
|---|---|
| per-thread `main_context`/`current_goroutine` (TLS) | 2 (steps 1,3,4,5) |
| deferred stack-free (reap from scheduler) | 2 (steps 2,4) |
| DONE-only reap, yielded self-requeue intact | 2 (steps 4,5) |
| per-running-thread `uc_link` | 2 (step 4) |
| default stays 1 (no `goo_go` change) | (constraint; untouched) |
| RED→GREEN MT stress test, N=4, ≥50 iters | 1, 2 (BATCHES=100) |
| C-level test (probe can't request N) | 1 |
| wired into verify: + tests.yml | 3 |
| no regressions / local verification | 3 |
