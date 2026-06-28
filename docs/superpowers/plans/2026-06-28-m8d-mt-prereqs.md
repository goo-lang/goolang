# M8d — Pre-Default-Flip Concurrency Prerequisites Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the three §8a hardening items (channel-block bookkeeping accessor, `goo_yield` race fix, channel-under-N=4 coverage) without changing the single-threaded default.

**Architecture:** Runtime-only (plus one accessor declaration in `include/runtime.h`). Fix `goo_yield`'s save-before-publish race with a scheduler-side `t_requeue` handoff mirroring M8c's `t_reap`; expose `goo_current_goroutine()` over the per-thread `t_current` and route `channels.c` through it; add two C stress tests at `goo_scheduler_init(4)`.

**Tech Stack:** C23, ucontext, pthreads, C11 `_Thread_local` + `<stdatomic.h>`, GNU Make.

## Global Constraints

- **Default thread count stays 1.** Do NOT change `goo_go`'s lazy `goo_scheduler_init(1)`. (Spec §2.1)
- **Item 1 is hygiene only.** Make channel-wait bookkeeping per-thread-correct via the accessor; do NOT attempt to make the deadlock detector see channel-blocked goroutines (separate milestone). No "detector fixed" claim. (Spec §2.2, §3)
- **Item 2 mirrors `t_reap`.** Use a new `_Thread_local t_requeue` set by `goo_yield`, acted on by the scheduler after the swap. No new synchronization mechanism. (Spec §2.3, §4)
- **Header change = `make clean`.** The accessor adds a declaration to `include/runtime.h`, so rebuild with `make clean && make goo lib/libgoo_runtime.a` after Task 3. Other tasks use `make goo lib/libgoo_runtime.a`.
- **Run all concurrency tests under a timeout** so a hang fails loudly.
- **Shell is zsh:** pass make target lists literally (an unquoted `$VAR` does NOT word-split).
- **CI billing caveat:** Actions on `dd0wney/goolang` may be billing-blocked; authoritative verification is LOCAL (probe list + `make test`).
- **clang/LSP false positives:** "header not found"/"unknown type" diagnostics are not real (build uses `-Iinclude`). Trust `make`.

## Reference map

| Concern | Location | Notes |
|---|---|---|
| per-thread state + reap block | `src/runtime/concurrency.c:41-43` (TLS), `:251-281` (loop run/reap) | add `t_requeue`; `t_reap=NULL` at `:252`; reap block at `:276-281` |
| `goo_yield` | `src/runtime/concurrency.c:186-205` | currently `scheduler_add_goroutine(current)` at `:193` then swap |
| `scheduler_add_goroutine` | `src/runtime/concurrency.c` | locked enqueue helper (reuse) |
| channels send-wait bookkeeping | `src/runtime/channels.c:241-256` | mark/clear via `g_scheduler->current_goroutine` |
| channels recv-wait bookkeeping | `src/runtime/channels.c:358-373` | mark/clear via `g_scheduler->current_goroutine` |
| runtime API decls (accessor spot) | `include/runtime.h:173-176` | after `void goo_goroutine_exit(void);` (`:176`) |
| goroutine fields | `include/runtime.h` | `waiting_on_channel`, `waiting_for_send`, `state`; `ucontext_t context` |
| channel C API | `include/runtime.h:179-185` | `goo_make_chan(size_t elem,size_t buf)`, `goo_chan_send(ch,void*)`, `goo_chan_recv(ch,void*)` |
| stress-test build pattern | M8c | `$(CC) -std=c23 -D_GNU_SOURCE -Iinclude -I. T.c lib/libgoo_runtime.a -lpthread -lm` |
| existing stress target to mirror | `Makefile` `mt-scheduler-stress:` | recipe shape + `verify:` + tests.yml wiring |

---

## Task 1: `yield-stress` C test (RED)

**Files:**
- Create: `tests/concurrency/yield_stress.c`

**Interfaces:**
- Consumes: `goo_scheduler_init(int)`, `goo_go(goo_goroutine_func_t,void*)`, `goo_yield(void)`, `goo_scheduler_wait(void)`.
- Produces: `build/yield_stress` (built ad hoc here; Makefile target added in Task 4).

- [ ] **Step 1: Write the test**

Create `tests/concurrency/yield_stress.c`:

```c
// yield-stress: goroutines call goo_yield() repeatedly under 4 OS threads.
// RED before M8d Item 2: goo_yield re-enqueues the goroutine BEFORE swapcontext
// saves its context, so another worker can swap into a half-saved/duplicated
// context → crash/corruption. GREEN after: the scheduler re-enqueues via
// t_requeue only after the swap completes.
#define _GNU_SOURCE
#include "runtime.h"
#include <stdatomic.h>
#include <stdio.h>

static atomic_int g_done;

static void yielder(void* arg) {
    (void)arg;
    for (int i = 0; i < 50; i++) {
        goo_yield();
    }
    atomic_fetch_add(&g_done, 1);
}

int main(void) {
    goo_scheduler_init(4);
    const int BATCHES = 50, PER = 16;
    for (int b = 0; b < BATCHES; b++) {
        atomic_store(&g_done, 0);
        for (int i = 0; i < PER; i++) {
            goo_go(yielder, NULL);
        }
        goo_scheduler_wait();
        int got = atomic_load(&g_done);
        if (got != PER) {
            fprintf(stderr, "batch %d: done=%d expected %d\n", b, got, PER);
            return 1;
        }
    }
    printf("yield-stress OK (%d batches x %d goroutines x 50 yields @ 4 threads)\n", BATCHES, PER);
    return 0;
}
```

- [ ] **Step 2: Build runtime + compile the test**

Run:
```bash
make goo lib/libgoo_runtime.a
mkdir -p build
gcc -std=c23 -D_GNU_SOURCE -Iinclude -I. tests/concurrency/yield_stress.c lib/libgoo_runtime.a -lpthread -lm -o build/yield_stress
```
Expected: compiles. (Use `gcc-14` if `gcc` is unavailable.)

- [ ] **Step 3: Run several times to confirm RED**

Run:
```bash
ok=0; bad=0; for i in $(seq 1 20); do timeout 30 ./build/yield_stress >/dev/null 2>&1 && ok=$((ok+1)) || bad=$((bad+1)); done; echo "OK=$ok BAD=$bad / 20"
```
Expected: **BAD > 0** (crash / hang / wrong count) — the pre-fix `goo_yield` races at N=4. Record the numbers. (If 20/20 pass, raise the inner yield loop to 200 and BATCHES to 100 and re-run; the race is probabilistic but appears under enough yield churn.)

- [ ] **Step 4: Commit**

```bash
git add tests/concurrency/yield_stress.c
git commit --no-gpg-sign -m "test(runtime): add yield-stress (RED) — goo_yield races at num_threads=4"
```

---

## Task 2: Fix the `goo_yield` race (scheduler-side handoff) — GREEN

**Files:**
- Modify: `src/runtime/concurrency.c` (add `t_requeue`; rewrite `goo_yield`; extend the scheduler loop)

**Interfaces:**
- Consumes: `t_sched_ctx`, `t_current`, `t_reap` (existing TLS), `scheduler_add_goroutine`.
- Produces: race-free `goo_yield`. No signature change.

- [ ] **Step 1: Add the `t_requeue` thread-local**

In `src/runtime/concurrency.c`, immediately after the existing `static _Thread_local goo_goroutine_t* t_reap = NULL;` (line 43), add:

```c
// M8d: a yielding goroutine hands itself here; the scheduler re-enqueues it
// AFTER swapcontext has saved its context (mirrors t_reap). Re-enqueuing from
// inside goo_yield (before the swap) would publish the goroutine to other
// workers before its context is written → race. Mutually exclusive with t_reap.
static _Thread_local goo_goroutine_t* t_requeue = NULL;
```

- [ ] **Step 2: Rewrite `goo_yield` to hand off instead of self-enqueue**

Replace the body of `goo_yield` (`src/runtime/concurrency.c:186-205`) with:

```c
void goo_yield(void) {
    goo_goroutine_t* current = t_current;
    if (!g_scheduler || !current) {
        return;
    }

    current->state = GOO_GOROUTINE_READY;
    t_requeue = current;   // scheduler re-enqueues AFTER the swap saves our context

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

- [ ] **Step 3: Clear `t_requeue` before each run and re-enqueue after the swap**

In `scheduler_main_loop`, find the line `t_reap = NULL;` (line 252) and add `t_requeue = NULL;` right after it:

```c
            t_current = goroutine;
            t_reap = NULL;                 // cleared each run; the goroutine sets it on exit
            t_requeue = NULL;              // cleared each run; the goroutine sets it on yield
```

Then find the post-swap reap block (lines 276-281):

```c
            if (t_reap) {
                goo_free(t_reap->stack);
                goo_free(t_reap);
                t_reap = NULL;
            }
            t_current = NULL;
```

and insert the re-enqueue branch immediately before `t_current = NULL;`:

```c
            if (t_reap) {
                goo_free(t_reap->stack);
                goo_free(t_reap);
                t_reap = NULL;
            }
            // A yielded goroutine is published to the ready queue ONLY here —
            // after swapcontext has fully saved its context — so no other worker
            // can swap into a half-saved context.
            if (t_requeue) {
                scheduler_add_goroutine(t_requeue);
                t_requeue = NULL;
            }
            t_current = NULL;
```

- [ ] **Step 4: Build and confirm `yield-stress` is GREEN**

Run:
```bash
make goo lib/libgoo_runtime.a
gcc -std=c23 -D_GNU_SOURCE -Iinclude -I. tests/concurrency/yield_stress.c lib/libgoo_runtime.a -lpthread -lm -o build/yield_stress
ok=0; bad=0; for i in $(seq 1 30); do timeout 30 ./build/yield_stress >/dev/null 2>&1 && ok=$((ok+1)) || bad=$((bad+1)); done; echo "OK=$ok BAD=$bad / 30"
```
Expected: **OK=30 BAD=0**. If any BAD remains, STOP — the handoff is incomplete (re-check that `goo_yield` no longer calls `scheduler_add_goroutine` and that the loop re-enqueues `t_requeue` after the swap).

- [ ] **Step 5: Confirm no self-enqueue remains in `goo_yield`**

```bash
sed -n '/void goo_yield/,/^}/p' src/runtime/concurrency.c | grep -c "scheduler_add_goroutine" 
```
Expected: `0` (the enqueue moved to the scheduler loop).

- [ ] **Step 6: Commit**

```bash
git add src/runtime/concurrency.c
git commit --no-gpg-sign -m "fix(runtime): goo_yield re-enqueues via scheduler handoff after swap (close save-before-publish race)"
```

---

## Task 3: Item 1 — channel-block bookkeeping via `goo_current_goroutine()` accessor

**Files:**
- Modify: `include/runtime.h` (declare accessor), `src/runtime/concurrency.c` (define accessor), `src/runtime/channels.c` (route 2 blocks through it)

**Interfaces:**
- Consumes: `t_current` (file-static in `concurrency.c`).
- Produces: `goo_goroutine_t* goo_current_goroutine(void)` — returns the goroutine running on the calling worker (or NULL).

- [ ] **Step 1: Declare the accessor**

In `include/runtime.h`, immediately after `void goo_goroutine_exit(void);` (line 176), add:

```c
// Returns the goroutine currently running on the calling worker thread, or NULL
// if the caller is not inside a goroutine. (Per-thread; supersedes the old
// shared g_scheduler->current_goroutine field for channel-wait bookkeeping.)
goo_goroutine_t* goo_current_goroutine(void);
```

- [ ] **Step 2: Define the accessor**

In `src/runtime/concurrency.c`, immediately after the `goo_yield` function, add:

```c
goo_goroutine_t* goo_current_goroutine(void) {
    return t_current;
}
```

- [ ] **Step 3: Route the channels.c send-wait block through the accessor**

In `src/runtime/channels.c`, replace the send-wait block (lines 240-256, the `while (ch->length >= ch->capacity && !ch->closed) { ... }` body) with:

```c
        while (ch->length >= ch->capacity && !ch->closed) {
            // Mark the running goroutine as waiting for send. NOTE: this
            // bookkeeping is per-thread-correct but the deadlock detector still
            // cannot observe it — a channel-blocked goroutine parks in the
            // pthread_cond_wait below, not in g_scheduler->ready_queue (which is
            // all the detector walks). Working channel-deadlock detection needs
            // a separate blocked-set and is a future milestone.
            goo_goroutine_t* self = goo_current_goroutine();
            if (self) {
                self->waiting_on_channel = ch;
                self->waiting_for_send = 1;
                self->state = GOO_GOROUTINE_BLOCKED;
            }

#ifdef GOO_PLATFORM_UNIX
            pthread_cond_wait(&ch->not_full->cond, &ch->mutex->mutex);
#endif

            // Clear waiting state when unblocked.
            if (self) {
                self->waiting_on_channel = NULL;
                self->waiting_for_send = 0;
                self->state = GOO_GOROUTINE_RUNNING;
            }
        }
```

- [ ] **Step 4: Route the channels.c recv-wait block through the accessor**

In `src/runtime/channels.c`, replace the recv-wait block (lines 357-373, the `while (ch->length == 0 && !ch->closed) { ... }` body) with:

```c
        while (ch->length == 0 && !ch->closed) {
            // See the send-wait note above re: deadlock-detector visibility.
            goo_goroutine_t* self = goo_current_goroutine();
            if (self) {
                self->waiting_on_channel = ch;
                self->waiting_for_send = 0;
                self->state = GOO_GOROUTINE_BLOCKED;
            }

#ifdef GOO_PLATFORM_UNIX
            pthread_cond_wait(&ch->not_empty->cond, &ch->mutex->mutex);
#endif

            // Clear waiting state when unblocked.
            if (self) {
                self->waiting_on_channel = NULL;
                self->waiting_for_send = 0;
                self->state = GOO_GOROUTINE_RUNNING;
            }
        }
```

- [ ] **Step 5: Rebuild (header changed) and verify channel probes + grep**

Run:
```bash
make clean && make goo lib/libgoo_runtime.a
grep -c "g_scheduler->current_goroutine" src/runtime/channels.c    # expect 0
make chan-probe chan-elem-probe chan-padded-probe chan-uint-probe unbuffered-probe 2>&1 | grep -E "PASS|FAIL"
```
Expected: grep `0` (channels.c no longer reads the shared field); every channel probe `PASS` (no regression). The header change requires the `make clean`.

- [ ] **Step 6: Commit**

```bash
git add include/runtime.h src/runtime/concurrency.c src/runtime/channels.c
git commit --no-gpg-sign -m "refactor(runtime): channel-block bookkeeping via goo_current_goroutine() accessor (per-thread); document detector limitation"
```

---

## Task 4: `chan-mt-stress` test + wire all new targets + full regression

**Files:**
- Create: `tests/concurrency/chan_mt_stress.c`
- Modify: `Makefile` (add `yield-stress:`, `chan-mt-stress:`; append both to `verify:`), `.github/workflows/tests.yml` (append both)

**Interfaces:**
- Consumes: `goo_scheduler_init(int)`, `goo_go`, `goo_make_chan(size_t,size_t)`, `goo_chan_send(goo_channel_t*,void*)`, `goo_chan_recv(goo_channel_t*,void*)`, `goo_current_goroutine()` (from Task 3).
- Produces: CI-gated `yield-stress` + `chan-mt-stress`.

- [ ] **Step 1: Write the channel-MT stress test (also exercises the accessor)**

Create `tests/concurrency/chan_mt_stress.c`:

```c
// chan-mt-stress: producer goroutines send over a shared buffered channel under
// 4 OS threads; main receives and checks the aggregate. Exercises channel
// send/recv under true parallelism (a gap left by mt-scheduler-stress) and the
// goo_current_goroutine() accessor. Expected GREEN on current code (regression
// guard); a failure here is a real channel-MT bug to fix.
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>

static goo_channel_t* g_ch;

static void producer(void* arg) {
    (void)arg;
    // accessor sanity: inside a goroutine this must be non-NULL.
    if (goo_current_goroutine() == NULL) {
        // signal failure by sending a sentinel the checker will catch
        int bad = -1000000;
        goo_chan_send(g_ch, &bad);
        return;
    }
    int v = 7;
    goo_chan_send(g_ch, &v);
}

int main(void) {
    goo_scheduler_init(4);
    const int BATCHES = 50, PER = 16;
    for (int b = 0; b < BATCHES; b++) {
        g_ch = goo_make_chan(sizeof(int), PER);   // buffered, capacity PER
        for (int i = 0; i < PER; i++) {
            goo_go(producer, NULL);
        }
        long sum = 0;
        for (int i = 0; i < PER; i++) {
            int got = 0;
            goo_chan_recv(g_ch, &got);
            sum += got;
        }
        goo_scheduler_wait();
        if (sum != (long)PER * 7) {
            fprintf(stderr, "batch %d: sum=%ld expected %d\n", b, sum, PER * 7);
            return 1;
        }
    }
    printf("chan-mt-stress OK (%d batches x %d producers @ 4 threads)\n", BATCHES, PER);
    return 0;
}
```

- [ ] **Step 2: Compile and run it (expect GREEN)**

Run:
```bash
make goo lib/libgoo_runtime.a
gcc -std=c23 -D_GNU_SOURCE -Iinclude -I. tests/concurrency/chan_mt_stress.c lib/libgoo_runtime.a -lpthread -lm -o build/chan_mt_stress
ok=0; bad=0; for i in $(seq 1 20); do timeout 30 ./build/chan_mt_stress >/dev/null 2>&1 && ok=$((ok+1)) || bad=$((bad+1)); done; echo "OK=$ok BAD=$bad / 20"
```
Expected: **OK=20 BAD=0**. If BAD>0, a latent channel-MT bug exists — STOP and report it (do not commit a red gate); it must be root-caused (systematic-debugging) before this milestone closes.

- [ ] **Step 3: Add both Makefile targets**

In `Makefile`, after the existing `mt-scheduler-stress:` target, add (TAB-indented, matching that target's style):

```make
# M8d: goo_yield correctness under real multi-threading.
yield-stress: $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== yield-stress: goo_yield safe under num_threads=4 ==="
	$(CC) -std=c23 -D_GNU_SOURCE -Iinclude -I. tests/concurrency/yield_stress.c $(RUNTIME_LIB) -lpthread -lm -o build/yield_stress
	@timeout 60 ./build/yield_stress; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "yield-stress: PASS"; else echo "yield-stress: FAIL (exit $$rc)"; exit 1; fi

# M8d: channel send/recv correctness under real multi-threading.
chan-mt-stress: $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== chan-mt-stress: channel send/recv correct under num_threads=4 ==="
	$(CC) -std=c23 -D_GNU_SOURCE -Iinclude -I. tests/concurrency/chan_mt_stress.c $(RUNTIME_LIB) -lpthread -lm -o build/chan_mt_stress
	@timeout 60 ./build/chan_mt_stress; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "chan-mt-stress: PASS"; else echo "chan-mt-stress: FAIL (exit $$rc)"; exit 1; fi
```

- [ ] **Step 4: Wire into `verify:` and `tests.yml`**

In `Makefile`, append ` yield-stress chan-mt-stress` to the end of the `verify:` dependency list.
In `.github/workflows/tests.yml`, append ` yield-stress chan-mt-stress` to the end of the space-separated make-target list (the line containing `mt-scheduler-stress`).

- [ ] **Step 5: Run the new targets + full regression**

Run (literal target list):
```bash
make yield-stress chan-mt-stress mt-scheduler-stress go-probe chan-probe chan-elem-probe chan-padded-probe chan-uint-probe unbuffered-probe select-probe block-scope-probe escape-probe escape-range-probe 2>&1 | grep -E "PASS|FAIL"
make test 2>&1 | grep -E "Passed:|Failed|All tests"
```
Expected: every target `PASS`, no `FAIL`; `make test` 76 passed / 1 skipped.

- [ ] **Step 6: Commit**

```bash
git add tests/concurrency/chan_mt_stress.c Makefile .github/workflows/tests.yml
git commit --no-gpg-sign -m "test(runtime): chan-mt-stress + gate yield-stress/chan-mt-stress in verify: and tests.yml"
```

---

## Final verification

- [ ] `yield-stress` RED before Task 2, GREEN after (30/30, exit 0).
- [ ] `goo_yield` no longer self-enqueues; the scheduler re-enqueues via `t_requeue` after the swap (Task 2 Step 5 grep `0`).
- [ ] `channels.c` uses `goo_current_goroutine()`, not `g_scheduler->current_goroutine` (Task 3 Step 5 grep `0`); detector limitation documented in code + spec.
- [ ] `chan-mt-stress` GREEN under N=4 (20/20).
- [ ] Default unchanged (`num_threads=1`); full probe suite + `make test` (76/1) green.
- [ ] New targets wired into `verify:` + `tests.yml`.
- [ ] Spec §8 success criteria met.

## Spec coverage self-check

| Spec §8 / item | Task |
|---|---|
| Item 1: accessor + channels.c routing | 3 |
| Item 1: detector limitation documented | 3 (code comment), spec §3 |
| Item 2: goo_yield handoff via t_requeue | 2 |
| Item 2: yield-stress RED→GREEN | 1, 2 |
| Item 3: chan-mt-stress under N=4 | 4 |
| default stays 1 (no goo_go change) | (constraint; untouched) |
| wired into verify: + tests.yml | 4 |
| no regressions / local verification | 4 |
