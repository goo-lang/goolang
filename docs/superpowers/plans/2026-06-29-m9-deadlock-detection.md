# M9 — Channel-Deadlock Detection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a fully-deadlocked Goo program print `fatal error: all goroutines are asleep - deadlock!` and exit 2 instead of hanging, via a Go-faithful block-time global "all asleep" check.

**Architecture:** Runtime-only. Account blocked goroutines + a `main_in_wait` flag in the scheduler; at each channel `cond_wait` block point and in `goo_scheduler_wait`'s poll loop, evaluate "all participants asleep" under `scheduler_mutex`; on a positive verdict abort with Go's message + `exit(2)`. The broken `ready_queue`-scanning cycle detector is removed.

**Tech Stack:** C23, pthreads, GNU Make.

## Global Constraints

- **Block-time global all-asleep model** (Go's `checkdead`); NOT subset-cycle detection. (Spec §1, §3)
- **Two evaluation sites, same predicate** `blocked_goroutines == num_goroutines && ready_queue == NULL`: at `goo_sched_block_begin` (goroutine path additionally requires `main_in_wait`; main path does not) and in `goo_scheduler_wait`'s poll loop (requires `num_goroutines > 0`). The poll-loop site closes the race where the last goroutine blocks before `main` sets `main_in_wait`. (Spec §3.3)
- **On detection:** write exactly `fatal error: all goroutines are asleep - deadlock!\n` to stderr, then `exit(2)`. (Spec §3.4)
- **No false positives** is the primary safety criterion: the full existing channel/go/escape/stress suite + `make test` must stay green. (Spec §5)
- **Lock order:** channel block points hold `ch->mutex`; the helpers take `scheduler_mutex` inside → order is `ch->mutex` then `scheduler_mutex` (safe; no reverse path exists). (Spec §3.5)
- **Default thread count unchanged** (still 1). Runtime-only; no grammar/codegen change.
- **Header change** (`goo_deadlock_detector_t` gains fields + new decls) ⇒ `make clean && make goo lib/libgoo_runtime.a`. Other tasks use `make goo lib/libgoo_runtime.a`.
- **Run concurrency tests under a timeout.** Shell is zsh — pass make target lists literally (unquoted `$VAR` does not word-split).
- **CI billing caveat:** authoritative verification is LOCAL. **clang/LSP "header not found"/"unknown type" diagnostics are false positives** — trust `make`.

## Reference map

| Concern | Location | Notes |
|---|---|---|
| detector struct (add fields) | `include/runtime.h:213-218` `goo_deadlock_detector_t` | `enabled`, `last_check_time`, `check_interval_ns`, `detected_deadlock` |
| scheduler API decls | `include/runtime.h:221-225` | `goo_deadlock_{init,shutdown,check,enable,detected}` |
| `goo_scheduler_wait` | `src/runtime/concurrency.c:119-137` | poll loop on `num_goroutines==0 && ready_queue==NULL` |
| idle-branch deadlock call (remove) | `src/runtime/concurrency.c:303` | `if (goo_deadlock_check()) break;` |
| dead detector to strip | `src/runtime/deadlock.c` | `goroutine_depends_on`, `detect_cycle_dfs`, `detect_deadlock`, `goo_deadlock_check`, `get_current_time_ns`, all `DEBUG`/`FATAL`/`WARNING` printfs |
| keep | `src/runtime/deadlock.c` | `goo_deadlock_{init,shutdown,enable,detected}` |
| channel block points (5) | `src/runtime/channels.c:254,291,309,372,407` | each is `pthread_cond_wait(...)` inside a `while` holding `ch->mutex` |
| accessor (caller identity) | `goo_current_goroutine()` (runtime.h) | NULL ⇒ caller is `main` (not a goroutine) |
| live count | `g_scheduler->stats.num_goroutines` | incremented in `goo_go`, decremented in `goo_goroutine_exit` |
| probe + custom Make target pattern | `Makefile` `mt-scheduler-stress:` | shape for asserting exit code + stderr |

---

## Task 1: Deadlock probes (RED — they hang today)

**Files:**
- Create: `examples/deadlock_probe.goo`, `examples/deadlock_goroutine_probe.goo`

**Interfaces:**
- Consumes: `make_chan`, channel receive `<-c`, `go`, `fmt.Println`.
- Produces: the two `.goo` programs the gate targets compile (targets added in Task 3).

- [ ] **Step 1: Write the main-path deadlock program**

Create `examples/deadlock_probe.goo`:

```go
// deadlock-probe: main receives on an empty buffered channel with no sender.
// Today: hangs forever. After M9: prints the deadlock message and exits 2.
package main

import "fmt"

func main() {
    c := make_chan(int, 1)
    v := <-c          // no sender — every participant is asleep
    fmt.Println(v)    // never reached
}
```

- [ ] **Step 2: Write the goroutine-path deadlock program**

Create `examples/deadlock_goroutine_probe.goo`:

```go
// deadlock-goroutine-probe: a spawned goroutine receives on an empty channel;
// main spawns it and ends (falls through to goo_scheduler_wait) without sending.
// Exercises the goroutine path + the scheduler-wait poll-loop check.
package main

import "fmt"

func consume(c chan int) {
    v := <-c
    fmt.Println(v)    // never reached
}

func main() {
    c := make_chan(int, 1)
    go consume(c)
    // main never sends; the goroutine is stuck forever
}
```

- [ ] **Step 3: Build and confirm both HANG (RED)**

Run:
```bash
make goo lib/libgoo_runtime.a
mkdir -p build
bin/goo -o build/deadlock_probe examples/deadlock_probe.goo
bin/goo -o build/deadlock_goroutine_probe examples/deadlock_goroutine_probe.goo
timeout 5 ./build/deadlock_probe; echo "main-path exit: $?"
timeout 5 ./build/deadlock_goroutine_probe; echo "goroutine-path exit: $?"
```
Expected (RED): both print **exit: 124** (timeout/hang) with NO deadlock message — there is no detection yet. Record this.

- [ ] **Step 4: Commit**

```bash
git add examples/deadlock_probe.goo examples/deadlock_goroutine_probe.goo
git commit --no-gpg-sign -m "test(runtime): add deadlock probes (RED) — all-asleep programs hang today"
```

---

## Task 2: Block-time deadlock detection (GREEN)

**Files:**
- Modify: `include/runtime.h` (struct fields + decls), `src/runtime/deadlock.c` (strip dead detector, add abort), `src/runtime/concurrency.c` (helpers + scheduler_wait + remove idle call), `src/runtime/channels.c` (bracket 5 block points)

**Interfaces:**
- Consumes: `goo_current_goroutine()`, `g_scheduler->{scheduler_mutex, ready_queue, stats.num_goroutines}`.
- Produces: `void goo_sched_block_begin(void)`, `void goo_sched_block_end(void)`, `void goo_deadlock_abort(void)`.

- [ ] **Step 1: Add detector fields**

In `include/runtime.h`, replace the `goo_deadlock_detector_t` struct (`:213-218`) with:

```c
typedef struct goo_deadlock_detector {
    int enabled;
    uint64_t last_check_time;
    uint64_t check_interval_ns;
    int detected_deadlock;
    int blocked_goroutines;   // M9: goroutines currently parked in a channel cond_wait
    int main_in_wait;         // M9: main has entered goo_scheduler_wait (body done)
} goo_deadlock_detector_t;
```

- [ ] **Step 2: Declare the new functions; drop `goo_deadlock_check` decl**

In `include/runtime.h`, in the deadlock API block (`:221-225`), remove the `int goo_deadlock_check(void);` declaration and add:

```c
void goo_sched_block_begin(void);
void goo_sched_block_end(void);
void goo_deadlock_abort(void) __attribute__((noreturn));
```

(Keep `goo_deadlock_init`/`shutdown`/`enable`/`detected`.)

- [ ] **Step 3: Strip the dead detector and add the abort**

In `src/runtime/deadlock.c`, delete `get_current_time_ns`, `goroutine_depends_on`, `detect_cycle_dfs`, `detect_deadlock`, and `goo_deadlock_check` (and the now-unneeded `<time.h>` use if it triggers an unused-include warning — leaving the include is fine). Keep `goo_deadlock_init`/`shutdown`/`enable`/`detected`. Add:

```c
void goo_deadlock_abort(void) {
    fprintf(stderr, "fatal error: all goroutines are asleep - deadlock!\n");
    fflush(stderr);
    exit(2);
}
```

(`<stdio.h>` and `<stdlib.h>` are already included in this file.)

- [ ] **Step 4: Add the accounting helpers in concurrency.c**

In `src/runtime/concurrency.c`, after `goo_current_goroutine` (the accessor added in M8d), add:

```c
// M9: called by a participant immediately before it blocks on a channel
// (cond_wait), while holding that channel's mutex. Accounts the block and, if
// this makes every participant asleep, aborts with the deadlock message.
void goo_sched_block_begin(void) {
    if (!g_scheduler) return;
    int is_goroutine = (goo_current_goroutine() != NULL);

    goo_mutex_lock(g_scheduler->scheduler_mutex);
    if (is_goroutine) {
        g_scheduler->deadlock_detector.blocked_goroutines++;
    }
    int all_asleep = (g_scheduler->deadlock_detector.blocked_goroutines ==
                      (int)g_scheduler->stats.num_goroutines) &&
                     (g_scheduler->ready_queue == NULL);
    // Goroutine path: only a deadlock if main can no longer act (it's in
    // goo_scheduler_wait). Main path: main itself blocking is the last event.
    int deadlock = is_goroutine
        ? (all_asleep && g_scheduler->deadlock_detector.main_in_wait)
        : all_asleep;
    goo_mutex_unlock(g_scheduler->scheduler_mutex);

    if (deadlock) {
        goo_deadlock_abort();
    }
}

// M9: called by a goroutine immediately after it wakes from a channel cond_wait.
void goo_sched_block_end(void) {
    if (!g_scheduler) return;
    if (goo_current_goroutine() == NULL) return;  // main is not counted
    goo_mutex_lock(g_scheduler->scheduler_mutex);
    if (g_scheduler->deadlock_detector.blocked_goroutines > 0) {
        g_scheduler->deadlock_detector.blocked_goroutines--;
    }
    goo_mutex_unlock(g_scheduler->scheduler_mutex);
}
```

- [ ] **Step 5: Set `main_in_wait` and add the poll-loop check in `goo_scheduler_wait`**

In `src/runtime/concurrency.c`, replace `goo_scheduler_wait` (`:119-137`) with:

```c
void goo_scheduler_wait(void) {
    if (!g_scheduler) {
        return;  // No goroutines were ever started.
    }

    goo_mutex_lock(g_scheduler->scheduler_mutex);
    g_scheduler->deadlock_detector.main_in_wait = 1;  // main's body is done
    goo_mutex_unlock(g_scheduler->scheduler_mutex);

    for (;;) {
        goo_mutex_lock(g_scheduler->scheduler_mutex);
        int done = (g_scheduler->stats.num_goroutines == 0 &&
                    g_scheduler->ready_queue == NULL);
        int stopped = !g_scheduler->running;
        // All live goroutines asleep, none runnable, and main is here (can't act)
        // → deadlock. Closes the race where the last goroutine blocked before
        // main_in_wait was set (so goo_sched_block_begin did not fire).
        int deadlock = (g_scheduler->stats.num_goroutines > 0) &&
                       (g_scheduler->deadlock_detector.blocked_goroutines ==
                        (int)g_scheduler->stats.num_goroutines) &&
                       (g_scheduler->ready_queue == NULL);
        goo_mutex_unlock(g_scheduler->scheduler_mutex);

        if (deadlock) {
            goo_deadlock_abort();
        }
        if (done || stopped) {
            break;
        }

        goo_platform_sleep_ns(500000);  // 0.5ms
    }
}
```

- [ ] **Step 6: Remove the vestigial idle-branch deadlock call**

In `src/runtime/concurrency.c`, in `scheduler_main_loop`'s `else` branch (`:301-310`), replace:

```c
        } else {
            // No goroutines ready, check for deadlock
            if (goo_deadlock_check()) {
                // Deadlock detected, stop scheduler
                break;
            }
            
            // Sleep briefly
            goo_platform_sleep_ns(1000000);  // 1ms
        }
```

with (detection now happens at block points / in goo_scheduler_wait):

```c
        } else {
            // No goroutine ready; idle briefly. (Deadlock detection happens at
            // channel block points and in goo_scheduler_wait, not here.)
            goo_platform_sleep_ns(1000000);  // 1ms
        }
```

- [ ] **Step 7: Bracket the 5 channel block points**

In `src/runtime/channels.c`, at EACH of the five `pthread_cond_wait(...)` calls (`:254`, `:291`, `:309`, `:372`, `:407`), wrap the wait with the helpers — e.g. for `:254`:

```c
            goo_sched_block_begin();
            pthread_cond_wait(&ch->not_full->cond, &ch->mutex->mutex);
            goo_sched_block_end();
```

Apply the identical `goo_sched_block_begin();` / `goo_sched_block_end();` bracket around the other four `pthread_cond_wait(&ch->not_full->cond, ...)` / `pthread_cond_wait(&ch->not_empty->cond, ...)` calls. Do not change anything else in those loops.

- [ ] **Step 8: Build (header changed) and confirm both probes GREEN**

Run:
```bash
make clean && make goo lib/libgoo_runtime.a
bin/goo -o build/deadlock_probe examples/deadlock_probe.goo
bin/goo -o build/deadlock_goroutine_probe examples/deadlock_goroutine_probe.goo
for p in deadlock_probe deadlock_goroutine_probe; do
  timeout 10 ./build/$p 2>/tmp/$p.err; rc=$?
  echo "$p: exit=$rc"; grep -q "all goroutines are asleep - deadlock!" /tmp/$p.err && echo "  message: OK" || echo "  message: MISSING"
done
```
Expected: each `exit=2` (NOT 124) with `message: OK`. If either hangs (124) or is missing the message, STOP and report — do not proceed.

- [ ] **Step 9: Confirm NO false positives on existing channel programs**

Run:
```bash
make go-probe chan-probe chan-elem-probe chan-padded-probe chan-uint-probe unbuffered-probe select-probe escape-probe escape-range-probe 2>&1 | grep -E "PASS|FAIL"
gcc -std=c23 -D_GNU_SOURCE -Iinclude -I. tests/concurrency/chan_mt_stress.c lib/libgoo_runtime.a -lpthread -lm -o build/cms && ok=0; for i in $(seq 1 10); do timeout 30 ./build/cms >/dev/null 2>&1 && ok=$((ok+1)); done; echo "chan-mt-stress: $ok/10 (must be 10/10 — no spurious deadlock)"
```
Expected: every probe PASS; `chan-mt-stress 10/10`. A spurious `deadlock!` abort here is a FAIL — STOP and report.

- [ ] **Step 10: Commit**

```bash
git add include/runtime.h src/runtime/deadlock.c src/runtime/concurrency.c src/runtime/channels.c
git commit --no-gpg-sign -m "feat(runtime): block-time all-asleep deadlock detection (Go-faithful); strip dead cycle detector"
```

---

## Task 3: Wire the gate + full regression

**Files:**
- Create: `examples/deadlock_probe.expected.txt` is NOT used (custom targets assert exit code + stderr); none.
- Modify: `Makefile` (add `deadlock-probe:` + `deadlock-goroutine-probe:`; append to `verify:`), `.github/workflows/tests.yml` (append both)

**Interfaces:**
- Consumes: the two `.goo` probes; `$(COMPILER)`, `$(RUNTIME_LIB)`.
- Produces: CI-gated deadlock probes.

- [ ] **Step 1: Add the two Makefile targets**

In `Makefile`, after the existing `chan-mt-stress:` target, add (TAB-indented; a deadlock probe PASSES iff it exits 2 — not 124/hang — with the message on stderr):

```make
# M9: a fully-deadlocked program aborts with Go's message + exit 2 (not a hang).
deadlock-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== deadlock-probe: main blocked on empty channel aborts (exit 2) ==="
	$(COMPILER) -o build/deadlock_probe examples/deadlock_probe.goo
	@timeout 10 ./build/deadlock_probe 2>build/deadlock_probe.err; rc=$$?; \
	if [ $$rc -eq 124 ]; then echo "deadlock-probe: FAIL (hang — no detection)"; cat build/deadlock_probe.err; exit 1; fi; \
	if [ $$rc -ne 2 ]; then echo "deadlock-probe: FAIL (exit $$rc, expected 2)"; cat build/deadlock_probe.err; exit 1; fi; \
	if grep -q "all goroutines are asleep - deadlock!" build/deadlock_probe.err; then echo "deadlock-probe: PASS"; else echo "deadlock-probe: FAIL (missing message)"; cat build/deadlock_probe.err; exit 1; fi

deadlock-goroutine-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== deadlock-goroutine-probe: blocked goroutine + idle main aborts (exit 2) ==="
	$(COMPILER) -o build/deadlock_goroutine_probe examples/deadlock_goroutine_probe.goo
	@timeout 10 ./build/deadlock_goroutine_probe 2>build/deadlock_goroutine_probe.err; rc=$$?; \
	if [ $$rc -eq 124 ]; then echo "deadlock-goroutine-probe: FAIL (hang — no detection)"; cat build/deadlock_goroutine_probe.err; exit 1; fi; \
	if [ $$rc -ne 2 ]; then echo "deadlock-goroutine-probe: FAIL (exit $$rc, expected 2)"; cat build/deadlock_goroutine_probe.err; exit 1; fi; \
	if grep -q "all goroutines are asleep - deadlock!" build/deadlock_goroutine_probe.err; then echo "deadlock-goroutine-probe: PASS"; else echo "deadlock-goroutine-probe: FAIL (missing message)"; cat build/deadlock_goroutine_probe.err; exit 1; fi
```

- [ ] **Step 2: Wire into `verify:` and `tests.yml`**

In `Makefile`, append ` deadlock-probe deadlock-goroutine-probe` to the end of the `verify:` dependency list.
In `.github/workflows/tests.yml`, append ` deadlock-probe deadlock-goroutine-probe` to the end of the make-target list (the line containing `chan-mt-stress`).

- [ ] **Step 3: Run the new targets + full regression**

Run (literal target list):
```bash
make deadlock-probe deadlock-goroutine-probe yield-stress chan-mt-stress mt-scheduler-stress go-probe chan-probe chan-elem-probe chan-padded-probe chan-uint-probe unbuffered-probe select-probe block-scope-probe escape-probe escape-range-probe 2>&1 | grep -E "PASS|FAIL"
make test 2>&1 | grep -E "Passed:|Failed|All tests"
```
Expected: every target `PASS`, no `FAIL`; `make test` 76 passed / 1 skipped. (The no-false-positive criterion: every non-deadlock probe must still PASS.)

- [ ] **Step 4: Commit**

```bash
git add Makefile .github/workflows/tests.yml
git commit --no-gpg-sign -m "ci(runtime): gate deadlock-probe + deadlock-goroutine-probe in verify: and tests.yml"
```

---

## Final verification

- [ ] `deadlock-probe` + `deadlock-goroutine-probe`: exit 2 (not 124) with stderr `fatal error: all goroutines are asleep - deadlock!` — both hung before Task 2.
- [ ] No false positives: full channel/go/escape/stress suite + `make test` (76/1) green.
- [ ] Dead `detect_deadlock`/`detect_cycle_dfs`/`goroutine_depends_on`/`goo_deadlock_check` + DEBUG/FATAL printfs removed.
- [ ] Default thread count unchanged; probes wired into `verify:` + `tests.yml`.
- [ ] Spec §7 success criteria met.

## Spec coverage self-check

| Spec §7 / element | Task |
|---|---|
| main-path all-asleep detection → exit 2 + message | 1, 2 (block_begin main path) |
| goroutine-path detection (block_begin + scheduler_wait poll) | 1, 2 (steps 4,5) |
| no false positives (suite green) | 2 (step 9), 3 (step 3) |
| strip dead detector + printfs | 2 (step 3) |
| default unchanged; wired into verify:/tests.yml | 3 |
| local verification | 2, 3 |
