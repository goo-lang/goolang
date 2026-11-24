# Deterministic Simulation of the Runtime Scheduler — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run the real scheduler and channel code under a seeded, single-threaded simulator that replays any failure from its seed, and prove it on the task 8 false deadlock before deciding whether it becomes a gate.

**Architecture:** One seam (`include/goo_os.h`) separates the runtime's logic from the OS: mutex, condvar, thread, sleep, clock, and the per-worker thread-local struct. Production links `src/runtime/goo_os_posix.c`; the simulator links `tests/sim/sim_os.c`, which runs every "OS thread" as a coroutine on one real thread, switches only at seam calls by a seeded choice, keeps a virtual clock, and records a trace. A shell driver runs one process per seed and replays every non-pass.

**Tech Stack:** C23, `ucontext`, `fork` via the shell driver, GNU make, bash. No new dependency.

**Spec:** `docs/superpowers/specs/2026-09-03-deterministic-simulation-design.md`. The plan argues from the spec; read both.

## Global Constraints

- The runtime's logic does not change. Task 1-3 are behaviour-neutral by measurement: every concurrency gate, the four stress gates, and `make verify-core` green before and after; the production archive's member list changes only by the added `goo_os_posix.o`.
- After task 3, `src/runtime/concurrency.c` and `src/runtime/channels.c` contain no `pthread_` token and no `_Thread_local` token. `runtime-seam-probe` enforces it with a positive control.
- Every new gate costs three companion edits in one commit (plan decision 2 of the verification-tooling plan): a `--self-test` in the script, `tools/parity-gate-count.txt` incremented for each new `VERIFY_ALL_DEPS` entry (227 today), and a regenerated `tests/probes/census.txt` where the base branch has one (this branch has none; say so in the report).
- Simulator limits, all compile-time constants in `tests/sim/sim.h`: `SIM_THREADS_MAX 17`, `SIM_STACK_BYTES 262144`, `SIM_WAITERS_MAX 17`, `SIM_MUTEXES_MAX 64`, `SIM_TRACE_ENTRIES 65536`. Run limits from the environment: `GOO_SIM_STEP_LIMIT` default `1000000`, `GOO_SIM_TIME_LIMIT_NS` default `10000000000`.
- Verdict exit codes: pass 0; runtime-deadlock-abort 2 (the runtime's own); sim-deadlock 10; invariant 11; step-limit 12; time-limit 13; replay-mismatch 14; limit-<name> 15.
- Seed budgets: 1,000 seeds per program for the spike; a fixed list of 256 seeds for the gate.
- `tests/sim/` follows Tiger Style: no allocation after `sim_init`; every loop bounded; no recursion; two assertions per function on average, positive and negative space, compound conditions split; explicit integer sizes (`uint32_t`, `uint64_t`); units last in names; functions of at most 70 lines; no dependency beyond libc, `ucontext.h` and the shell's `fork`.
- Commits: `git -c commit.gpgsign=false commit -F <message-file>`; the pre-commit hook runs `make test` (minutes; 600000 ms timeout); every message ends with the two attribution lines the session uses; conventional commits, imperative.
- Faults are off by default and off for both controls of the spike.
- Windows branches (`GOO_PLATFORM_WINDOWS`) move with their functions unchanged; nothing here is built or tested on Windows.

---

## File structure

| File | Responsibility |
|---|---|
| `include/goo_os.h` (new) | The seam: mutex, condvar, thread, sleep, clock, `goo_os_tls()`. The only header the runtime's logic includes for OS services. |
| `src/runtime/goo_os_posix.c` (new) | The production implementation. Receives the mutex functions from `sync.c`, the thread/time/sleep functions from `platform.c`, and the new condvar functions. |
| `src/runtime/sync.c` (modified) | Keeps waitgroups and stats, rewritten over the seam's mutex and condvar. |
| `src/runtime/platform.c` (modified) | Keeps `goo_platform_alloc`/`goo_platform_free` only. |
| `src/runtime/channels.c` (modified) | 18 raw condvar calls become seam calls. |
| `src/runtime/concurrency.c` (modified) | Worker threads through the seam; four thread-locals become one struct behind `goo_os_tls()`. |
| `scripts/runtime_seam_probe.sh` (new) | The grep gate with `--self-test`. |
| `tests/sim/sim.h` (new) | Limits, verdicts, the scheduler's public functions. |
| `tests/sim/sim_sched.c` (new) | Coroutines, seeded choice, blocking, virtual clock, trace ring, verdict exit. |
| `tests/sim/sim_os.c` (new) | The seam implemented over `sim_sched.c`. |
| `tests/sim/sim_main.c` (new) | `main`: environment, coroutine 0 runs `goo_sim_program_main`, the run loop. |
| `tests/sim/sim_sched_test.c` (new) | The scheduler's own tests, including the nesting measurement. |
| `tests/sim/sim_invariant_test.c` (new) | A program that unlocks a mutex it does not hold: the `invariant` control. |
| `scripts/sim_probe.sh` (new) | The driver: seeds, verdict lines, summary, replay, `--self-test`. |
| `tests/sim/README.md` (new) | How to run, read, and replay. The spike's numbers. |
| `Makefile` (modified) | `sim-runtime-lib`, `build/sim/%` rule, `sim-sched-test`, `runtime-seam-probe`, and, on a yes, `sim-false-deadlock-probe`. |

---

### Task 1: The seam header, the POSIX implementation, and the condvars in `channels.c`

**Files:**
- Create: `include/goo_os.h`
- Create: `src/runtime/goo_os_posix.c`
- Modify: `src/runtime/sync.c:14-70` (the five mutex functions move out)
- Modify: `src/runtime/platform.c:53-115,128-180` (thread, time, sleep move out)
- Modify: `src/runtime/channels.c` (18 call sites)
- Modify: `Makefile:89` (`RUNTIME_SRCS`) and `Makefile:140` (`RUNTIME_OBJS`)
- Test: the existing gates `chan-mt-stress`, `fanin-stress`, `false-deadlock-probe`, `deadlock-poll-probe`; `make verify-core`

**Interfaces:**
- Consumes: `struct goo_mutex` and `struct goo_cond` as declared in `include/runtime.h:752-765` (unchanged); `goo_thread_t`, `goo_thread_func_t` from `src/runtime/platform.h:54-57` (declarations move to `goo_os.h`, `platform.h` includes it).
- Produces, for every later task:

```c
/* include/goo_os.h — the seam. Production: src/runtime/goo_os_posix.c.
 * Simulator: tests/sim/sim_os.c. The two never link together. */
#ifndef GOO_OS_H
#define GOO_OS_H
#include <stdint.h>
#include <ucontext.h>
#include "runtime.h"        /* goo_mutex_t, goo_cond_t, goo_goroutine_t */

typedef struct goo_thread goo_thread_t;
typedef void* (*goo_thread_func_t)(void* arg);

/* Mutex: names unchanged, definitions move from sync.c. */
goo_mutex_t* goo_mutex_new(void);
void         goo_mutex_free(goo_mutex_t* mutex);
void         goo_mutex_lock(goo_mutex_t* mutex);
void         goo_mutex_unlock(goo_mutex_t* mutex);
int          goo_mutex_try_lock(goo_mutex_t* mutex);

/* Condvar: new. Replaces every pthread_cond_* in channels.c and sync.c. */
goo_cond_t*  goo_cond_new(void);
void         goo_cond_free(goo_cond_t* cond);
void         goo_cond_wait(goo_cond_t* cond, goo_mutex_t* mutex);
void         goo_cond_signal(goo_cond_t* cond);
void         goo_cond_broadcast(goo_cond_t* cond);

/* Threads: names unchanged, definitions move from platform.c. */
goo_thread_t* goo_thread_create(goo_thread_func_t func, void* arg);
void          goo_thread_join(goo_thread_t* thread);
void          goo_thread_detach(goo_thread_t* thread);

/* Clock and sleep: names unchanged, definitions move from platform.c. */
uint64_t goo_platform_time_ns(void);
uint64_t goo_platform_wall_time_ns(void);
void     goo_platform_sleep_ns(uint64_t ns);

/* The per-worker state that concurrency.c kept in four _Thread_local
 * variables. One real thread per worker in production; one coroutine per
 * worker in the simulator, each with its own copy. */
typedef struct goo_worker_tls {
    ucontext_t       sched_ctx;
    goo_goroutine_t* current;
    goo_goroutine_t* reap;
    goo_goroutine_t* requeue;
} goo_worker_tls_t;
goo_worker_tls_t* goo_os_tls(void);
#endif
```

- [ ] **Step 1: Record the baseline.** Run and save the output; these are the "before" measurements the task must match after.

Run:
```bash
cd <worktree>
make chan-mt-stress fanin-stress false-deadlock-probe deadlock-poll-probe > build/seam-before.log 2>&1; echo $?
grep -c ': PASS' build/seam-before.log
ar t lib/libgoo_runtime.a | sort > build/seam-before-members.txt; wc -l < build/seam-before-members.txt
```
Expected: exit 0; four PASS lines; 13 members.

- [ ] **Step 2: Write the seam header** `include/goo_os.h` exactly as in Interfaces above.

- [ ] **Step 3: Create `src/runtime/goo_os_posix.c`** by MOVING code, not rewriting it: cut `goo_mutex_new`, `goo_mutex_free`, `goo_mutex_lock`, `goo_mutex_unlock`, `goo_mutex_try_lock` from `src/runtime/sync.c:14-70` verbatim; cut `thread_wrapper`, `goo_thread_create`, `goo_thread_join`, `goo_thread_detach`, `goo_platform_time_ns`, `goo_platform_wall_time_ns`, `goo_platform_sleep_ns` from `src/runtime/platform.c` verbatim, with their `#ifdef GOO_PLATFORM_UNIX/WINDOWS` branches and `struct goo_thread` (find its definition in `platform.c` or `platform.h` and move it here as a file-local struct). Add the five condvar functions and the TLS accessor:

```c
goo_cond_t* goo_cond_new(void) {
    goo_cond_t* cond = goo_alloc(sizeof(goo_cond_t));
    GOO_ASSERT(cond != NULL);
#ifdef GOO_PLATFORM_UNIX
    int rc = pthread_cond_init(&cond->cond, NULL);
    GOO_ASSERT(rc == 0);
#endif
    return cond;
}
void goo_cond_free(goo_cond_t* cond) {
    GOO_ASSERT(cond != NULL);
#ifdef GOO_PLATFORM_UNIX
    int rc = pthread_cond_destroy(&cond->cond);
    GOO_ASSERT(rc == 0);
#endif
    goo_free(cond);
}
void goo_cond_wait(goo_cond_t* cond, goo_mutex_t* mutex) {
    GOO_ASSERT(cond != NULL);
    GOO_ASSERT(mutex != NULL);
#ifdef GOO_PLATFORM_UNIX
    int rc = pthread_cond_wait(&cond->cond, &mutex->mutex);
    GOO_ASSERT(rc == 0);
#endif
}
void goo_cond_signal(goo_cond_t* cond) {
    GOO_ASSERT(cond != NULL);
#ifdef GOO_PLATFORM_UNIX
    int rc = pthread_cond_signal(&cond->cond);
    GOO_ASSERT(rc == 0);
#endif
}
void goo_cond_broadcast(goo_cond_t* cond) {
    GOO_ASSERT(cond != NULL);
#ifdef GOO_PLATFORM_UNIX
    int rc = pthread_cond_broadcast(&cond->cond);
    GOO_ASSERT(rc == 0);
#endif
}
static _Thread_local goo_worker_tls_t t_worker;
goo_worker_tls_t* goo_os_tls(void) {
    return &t_worker;
}
```

Use whatever the file's existing allocation and assertion helpers are (`goo_alloc`/`goo_free` and `GOO_ASSERT` are the names in `include/runtime.h` and `include/goo_assert.h`; confirm and match). `platform.h` keeps its `goo_platform_alloc`/`goo_platform_free` declarations and gains `#include "goo_os.h"` so existing includers still see the thread and clock names.

- [ ] **Step 4: Route `channels.c` through the seam.** The 18 sites have exactly these shapes; apply the mapping at each:

| Before | After |
|---|---|
| `pthread_cond_init(&ch->not_full->cond, NULL);` (preceded by an allocation of `ch->not_full`) | `ch->not_full = goo_cond_new();` (delete the separate allocation) |
| `pthread_cond_init(&ch->not_empty->cond, NULL);` | `ch->not_empty = goo_cond_new();` |
| `pthread_cond_destroy(&ch->not_full->cond);` (followed by a free of `ch->not_full`) | `goo_cond_free(ch->not_full);` (delete the separate free) |
| `pthread_cond_destroy(&ch->not_empty->cond);` | `goo_cond_free(ch->not_empty);` |
| `pthread_cond_wait(&ch->not_full->cond, &ch->mutex->mutex);` | `goo_cond_wait(ch->not_full, ch->mutex);` |
| `pthread_cond_wait(&ch->not_empty->cond, &ch->mutex->mutex);` | `goo_cond_wait(ch->not_empty, ch->mutex);` |
| `pthread_cond_broadcast(&ch->not_full->cond);` | `goo_cond_broadcast(ch->not_full);` |
| `pthread_cond_broadcast(&ch->not_empty->cond);` | `goo_cond_broadcast(ch->not_empty);` |
| `pthread_cond_signal(...)` (one site) | `goo_cond_signal(...)` on the same cond |

Replace `#include <pthread.h>` in `channels.c` with `#include "goo_os.h"`. Do the same rewrite in `sync.c` for the waitgroup's own condvar and mutex use (9 pthread sites there), so `sync.c` also compiles with no `pthread_` token.

- [ ] **Step 5: Wire the build.** `Makefile:89` `RUNTIME_SRCS` gains `$(SRCDIR)/runtime/goo_os_posix.c`; `Makefile:140` `RUNTIME_OBJS` gains `$(BUILDDIR)/runtime/goo_os_posix.o`. Check whether the Bazel `src/runtime/BUILD` on this branch lists runtime sources by name; if it does, add the file there too so the Bazel branch's later merge is one line, and say so in the report.

- [ ] **Step 6: Prove behaviour-neutral.**

Run:
```bash
make clean > /dev/null 2>&1; make lexer > build/seam-build.log 2>&1; echo $?
grep -c 'pthread_' src/runtime/channels.c src/runtime/sync.c
make chan-mt-stress fanin-stress false-deadlock-probe deadlock-poll-probe > build/seam-after.log 2>&1; echo $?
grep -c ': PASS' build/seam-after.log
ar t lib/libgoo_runtime.a | sort > build/seam-after-members.txt; diff build/seam-before-members.txt build/seam-after-members.txt
```
Expected: build exit 0; `channels.c:0` and `sync.c:0`; gates exit 0 with four PASS lines; the member diff shows exactly one added line, `goo_os_posix.o`.

- [ ] **Step 7: `make verify-core`** (600000 ms timeout; read the final line from a log; exit status with no pipe). Expected: `verify-core: ALL GREEN GATES PASSED`.

- [ ] **Step 8: Commit.**
```bash
git add include/goo_os.h src/runtime/goo_os_posix.c src/runtime/sync.c src/runtime/platform.c src/runtime/platform.h src/runtime/channels.c Makefile
git -c commit.gpgsign=false commit -F msg   # "refactor(runtime): one seam for mutex, condvar, thread and clock, and channels.c uses it"
```

---

### Task 2: Worker threads and thread-local state in `concurrency.c` through the seam

**Files:**
- Modify: `src/runtime/concurrency.c:44-52` (the four `_Thread_local` declarations), `:104-130` (worker creation and join), every use of `t_sched_ctx`, `t_current`, `t_reap`, `t_requeue`
- Modify: `include/runtime.h:890` (`pthread_t* os_threads` becomes `goo_thread_t** os_threads`)
- Test: `mt-scheduler-stress`, `yield-stress`, `deadlock-poll-probe`, `false-deadlock-probe`; `make verify-core`

**Interfaces:**
- Consumes: `goo_os_tls()`, `goo_thread_create`, `goo_thread_join` from Task 1.
- Produces: `concurrency.c` with no OS call and no thread-local of its own.

- [ ] **Step 1: Baseline.** `make mt-scheduler-stress yield-stress deadlock-poll-probe false-deadlock-probe > build/tls-before.log 2>&1; echo $?` — expect 0 and four PASS lines.

- [ ] **Step 2: Replace the thread-locals.** Delete the four declarations at `concurrency.c:45-52` (keep the comment that explains `t_requeue`, move it above the struct in `goo_os.h` or leave it as a comment at the first use). Then replace every use:

```bash
sed -i 's/\bt_sched_ctx\b/goo_os_tls()->sched_ctx/g; s/\bt_current\b/goo_os_tls()->current/g; s/\bt_reap\b/goo_os_tls()->reap/g; s/\bt_requeue\b/goo_os_tls()->requeue/g' src/runtime/concurrency.c
grep -c 'goo_os_tls()' src/runtime/concurrency.c     # record the count
grep -n '_Thread_local' src/runtime/concurrency.c     # must print nothing; positive control: grep -c '_Thread_local' src/runtime/goo_os_posix.c prints 1
```
Where a function takes the address of `t_sched_ctx` (`&t_sched_ctx` becomes `&goo_os_tls()->sched_ctx`), that is correct: the pointer is to the worker's own struct.

- [ ] **Step 3: Replace the worker threads.** At `concurrency.c:108-112` and `:128`:

```c
/* before */
g_scheduler->os_threads = goo_alloc(sizeof(pthread_t) * num_threads);
...
pthread_create(&g_scheduler->os_threads[i], NULL, scheduler_main_loop, (void*)(intptr_t)i);
...
pthread_join(g_scheduler->os_threads[i], NULL);

/* after */
g_scheduler->os_threads = goo_alloc(sizeof(goo_thread_t*) * num_threads);
...
g_scheduler->os_threads[i] = goo_thread_create(scheduler_main_loop, (void*)(intptr_t)i);
GOO_ASSERT(g_scheduler->os_threads[i] != NULL);   /* P10 rule 7: the return is checked */
...
goo_thread_join(g_scheduler->os_threads[i]);
```
Match the file's actual argument expression for the worker index. Change `include/runtime.h:890` to `goo_thread_t** os_threads;`. Replace `#include <pthread.h>` in `concurrency.c` with `#include "goo_os.h"`.

- [ ] **Step 4: Prove.** `grep -c 'pthread_\|_Thread_local' src/runtime/concurrency.c` must print 0 (positive control: the same grep on `src/runtime/goo_os_posix.c` prints a number above 0). Then the four gates from Step 1, then `make verify-core`; all green. Record the `pthread-return-probe` count line from verify-core: it must not rise.

- [ ] **Step 5: Commit.** `refactor(runtime): workers and thread-local state through the seam`.

---

### Task 3: The seam gate, `runtime-seam-probe`

**Files:**
- Create: `scripts/runtime_seam_probe.sh`
- Modify: `Makefile` (target `runtime-seam-probe` and its self-test in `VERIFY_ALL_DEPS`), `tools/parity-gate-count.txt` (227 → 229: the probe and its self-test)
- Test: the script's own `--self-test`; `./scripts/probe_teeth_probe.sh`; `tools/parity.sh` (or `tools/parity_test.sh`, whichever this branch has)

- [ ] **Step 1: Write the script**

```bash
#!/usr/bin/env bash
# runtime-seam-probe — the scheduler and channel logic reach the OS through
# include/goo_os.h only. A raw pthread_ call or a _Thread_local in these files
# escapes the simulator (tests/sim) silently, so this grep keeps the seam whole.
set -u
PROBE="runtime-seam-probe"
ROOT="${RUNTIME_SEAM_ROOT:-$(git rev-parse --show-toplevel 2>/dev/null || pwd)}"
FILES="src/runtime/concurrency.c src/runtime/channels.c src/runtime/sync.c"

scan() {  # $1 = root; prints offending lines; exit 0 if none
    local root="$1" hits
    hits="$(cd "$root" && grep -n -E 'pthread_|_Thread_local' $FILES | grep -v -E '^[^:]+:[0-9]+:\s*//' )"
    if [ -n "$hits" ]; then echo "$hits"; return 1; fi
    return 0
}

if [ "${1:-}" = "--self-test" ]; then
    SELF="$PROBE --self-test"
    W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
    mkdir -p "$W/src/runtime"
    for f in $FILES; do cp "$ROOT/$f" "$W/$f"; done
    # Control: the real tree is green.
    if ! scan "$W" >/dev/null; then echo "$SELF: FAIL (control: the real tree already has a raw call)"; exit 1; fi
    echo "    ok: the control copy is clean"
    # Mutant 1: a raw pthread call in channels.c must be caught.
    printf 'static void zz(void) { pthread_mutex_lock(0); }\n' >> "$W/src/runtime/channels.c"
    if scan "$W" >/dev/null; then echo "$SELF: FAIL (a raw pthread_mutex_lock in channels.c was not caught)"; exit 1; fi
    echo "    ok: a raw pthread_mutex_lock is caught"
    cp "$ROOT/src/runtime/channels.c" "$W/src/runtime/channels.c"
    # Mutant 2: a thread-local in concurrency.c must be caught.
    printf 'static _Thread_local int zz;\n' >> "$W/src/runtime/concurrency.c"
    if scan "$W" >/dev/null; then echo "$SELF: FAIL (a _Thread_local in concurrency.c was not caught)"; exit 1; fi
    echo "    ok: a _Thread_local is caught"
    echo "$SELF: PASS (3 files, 2 mutations red, control green)"
    exit 0
fi

if out="$(scan "$ROOT")"; then
    echo "$PROBE: PASS ($FILES: no pthread_ token, no _Thread_local, tree $ROOT)"
    exit 0
fi
echo "$PROBE: FAIL (raw OS use in the runtime logic files)"; echo "$out" | sed 's/^/    /'; exit 1
```

- [ ] **Step 2: Run it red first** on a scratch copy with one injected call (the self-test does this; run `bash scripts/runtime_seam_probe.sh --self-test` and confirm the two `ok:` lines and PASS), then `bash scripts/runtime_seam_probe.sh` on the tree: PASS naming the three files.

- [ ] **Step 3: Wire the Makefile.** Add, next to `warning-count-probe`:
```make
runtime-seam-probe:
	@bash scripts/runtime_seam_probe.sh
runtime-seam-selftest:
	@bash scripts/runtime_seam_probe.sh --self-test
```
Add both names to `VERIFY_ALL_DEPS`; set `tools/parity-gate-count.txt` to 229; run `./scripts/probe_teeth_probe.sh` (PASS, the count of scripts with teeth rises by one) and the parity test (PASS 229).

- [ ] **Step 4: Commit.** `feat(probes): runtime-seam-probe keeps the OS behind goo_os.h, with the three companion edits`.

---

### Task 4: The simulator's scheduler, clock and trace, with the nesting measurement

**Files:**
- Create: `tests/sim/sim.h`, `tests/sim/sim_sched.c`, `tests/sim/sim_sched_test.c`
- Modify: `Makefile` (target `sim-sched-test`)
- Test: `tests/sim/sim_sched_test.c` (no runtime linked)

**Interfaces (produces, used by Task 5 and 6):**

```c
/* tests/sim/sim.h */
#ifndef GOO_SIM_H
#define GOO_SIM_H
#include <stdint.h>
#include <ucontext.h>

#define SIM_THREADS_MAX    17u      /* GOO_MAX_OS_THREADS (16) workers + main */
#define SIM_STACK_BYTES    262144u
#define SIM_WAITERS_MAX    17u
#define SIM_MUTEXES_MAX    64u
#define SIM_TRACE_ENTRIES  65536u

enum sim_verdict {
    SIM_PASS = 0, SIM_RUNTIME_DEADLOCK = 2, SIM_DEADLOCK = 10, SIM_INVARIANT = 11,
    SIM_STEP_LIMIT = 12, SIM_TIME_LIMIT = 13, SIM_REPLAY_MISMATCH = 14, SIM_LIMIT = 15,
};
enum sim_state { SIM_RUNNABLE, SIM_WAIT_COND, SIM_WAIT_MUTEX, SIM_WAIT_JOIN, SIM_SLEEP, SIM_EXITED };

typedef struct sim_thread {
    ucontext_t       ctx;
    enum sim_state   state;
    uint32_t         id;
    const void*      wait_on;        /* the cond, mutex, or thread it waits for */
    uint64_t         wake_at_ns;     /* SIM_SLEEP only */
    void*          (*func)(void*);
    void*            arg;
    void*            result;
    unsigned char    stack[SIM_STACK_BYTES];
    /* Task 5 adds the goo_worker_tls_t copy here. */
} sim_thread_t;

void      sim_init(uint64_t seed, uint64_t step_limit, uint64_t time_limit_ns, const char* trace_dir);
uint32_t  sim_thread_spawn(void* (*func)(void*), void* arg);       /* returns id; SIM_LIMIT if full */
sim_thread_t* sim_current(void);
void      sim_yield_point(const char* what);      /* the scheduling point; every seam call ends here */
void      sim_block(enum sim_state why, const void* on);   /* park the current thread, then switch */
uint32_t  sim_wake_one(const void* on, enum sim_state why);   /* seed-chosen waiter; returns count woken (0 or 1) */
uint32_t  sim_wake_all(const void* on, enum sim_state why);
void      sim_sleep_until(uint64_t wake_at_ns);
uint64_t  sim_now_ns(void);
uint32_t  sim_random_below(uint32_t n);          /* seeded; n > 0 */
void      sim_exit_with(enum sim_verdict verdict, const char* detail);   /* writes the trace, never returns */
void      sim_run_until_done(void);              /* Task 5's main loop: returns when every thread exited */
#endif
```

- [ ] **Step 1: Write the failing test** `tests/sim/sim_sched_test.c`: three tests, each a function, run from `main` in order, asserting with `GOO_ASSERT` or plain `assert`:

```c
/* T1: two threads yield ten times each; the trace of seed 7 equals the trace of seed 7 run again. */
/* T2: seed 7 and seed 8 produce different first choices (at least one differs across 100 yields). */
/* T3, the nesting measurement: a thread creates a ucontext coroutine on its own stack the way
 *     concurrency.c does (getcontext/makecontext/swapcontext), enters it, and INSIDE it calls
 *     sim_yield_point("nested") three times while a second thread also yields; the coroutine
 *     returns to its thread, the thread exits, and the trace replays identically for seed 11. */
```
Each test forks (`fork()` then `waitpid`) so that a `sim_exit_with` in one test cannot end the process; the parent asserts the child's exit code is 0. Print one line per test: `sim_sched_test: T3 nesting: PASS (steps=<n>, trace bytes=<n>)`.

- [ ] **Step 2: Run it to see it fail** to build: `make sim-sched-test` (add the target: compiles `tests/sim/sim_sched.c tests/sim/sim_sched_test.c` with the runtime's `CFLAGS`, no runtime archive, into `build/sim/sim_sched_test`, runs it). Expected: a link error on the undefined `sim_*` symbols.

- [ ] **Step 3: Implement `tests/sim/sim_sched.c`.** The core, in this order (each function at most 70 lines):

```c
static sim_thread_t g_threads[SIM_THREADS_MAX];   /* static; never allocated after sim_init */
static uint32_t     g_thread_count;
static uint32_t     g_current;
static uint64_t     g_rng;                        /* xorshift64* state */
static uint64_t     g_step, g_step_limit;
static uint64_t     g_now_ns, g_time_limit_ns;
static ucontext_t   g_sched_ctx;                  /* the real thread's own context */
static struct { uint64_t step, now_ns; uint32_t thread; char what[12]; uint32_t choice; } g_trace[SIM_TRACE_ENTRIES];
static uint64_t     g_trace_count;                /* total appended; the ring keeps the last SIM_TRACE_ENTRIES */
static char         g_trace_dir[256];

uint32_t sim_random_below(uint32_t n) {
    GOO_ASSERT(n > 0);
    g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27;
    uint64_t r = g_rng * 2685821657736338717ULL;
    return (uint32_t)(r % n);
}
static void trace_append(const char* what, uint32_t choice) { /* fixed record into the ring; g_trace_count++ */ }
static uint32_t runnable_pick(void) {
    /* Count SIM_RUNNABLE threads (bounded by SIM_THREADS_MAX); if 0 return UINT32_MAX;
       else pick the k-th runnable with k = sim_random_below(count). */
}
static void clock_advance_to_earliest_sleeper(void) {
    /* Find the smallest wake_at_ns among SIM_SLEEP threads; GOO_ASSERT(it > g_now_ns);
       set g_now_ns to it; if g_now_ns > g_time_limit_ns, sim_exit_with(SIM_TIME_LIMIT, ...);
       mark every sleeper with wake_at_ns <= g_now_ns runnable. */
}
static void switch_to(uint32_t next) {
    /* GOO_ASSERT(g_threads[next].state == SIM_RUNNABLE); GOO_ASSERT(next != g_current || count == 1);
       uint32_t prev = g_current; g_current = next; swapcontext(&g_threads[prev].ctx, &g_threads[next].ctx); */
}
void sim_yield_point(const char* what) {
    g_step++;
    if (g_step > g_step_limit) sim_exit_with(SIM_STEP_LIMIT, what);
    uint32_t next = runnable_pick();
    if (next == UINT32_MAX) {
        /* nothing runnable: advance the clock, or it is a simulated deadlock */
        if (no sleeper) sim_exit_with(SIM_DEADLOCK, what);
        clock_advance_to_earliest_sleeper();
        next = runnable_pick();
        GOO_ASSERT(next != UINT32_MAX);
    }
    trace_append(what, next);
    switch_to(next);
}
```
`sim_block` sets the current thread's state and `wait_on`, then calls `sim_yield_point`. `sim_wake_one` collects the waiters on `on` with the given state (bounded scan of the table), picks one by seed, sets it runnable, returns 1, or returns 0 if none. `sim_wake_all` sets all of them runnable. `sim_sleep_until` sets `SIM_SLEEP` with the deadline and yields. `sim_thread_spawn` takes the next free slot (or `sim_exit_with(SIM_LIMIT, "threads")`), `getcontext` + `makecontext` on the slot's own stack with a trampoline that calls `func(arg)`, stores the result, marks `SIM_EXITED`, wakes joiners (`sim_wake_all(&thread, SIM_WAIT_JOIN)`), and yields forever. `sim_exit_with` writes the ring to `<trace_dir>/<seed>.trace` as text, one record per line, then `_exit(verdict)`; an `atexit` handler registered in `sim_init` writes the ring on the runtime's own `exit(2)` too, and a `SIGABRT` handler writes it on an assertion failure.

Every function: at least two assertions, positive and negative space (`GOO_ASSERT(state == SIM_RUNNABLE)` beside `GOO_ASSERT(state != SIM_EXITED)`).

- [ ] **Step 4: Run the tests.** `make sim-sched-test`. Expected: T1, T2, T3 PASS with their step counts. T3 is the spike's step 2: record its line in `tests/sim/README.md` (created in Task 7; keep the number in the report until then). If T3 fails, STOP and report BLOCKED with the trace: the spec names approach B as the fallback and the controller decides.

- [ ] **Step 5: Commit.** `feat(sim): the coroutine scheduler, virtual clock and trace, with the nesting measurement`.

---

### Task 5: The seam over the simulator, the process entry, and the sim archive

**Files:**
- Create: `tests/sim/sim_os.c`, `tests/sim/sim_main.c`
- Modify: `tests/sim/sim.h` (add `goo_worker_tls_t tls;` to `sim_thread_t`), `Makefile` (`SIM_RUNTIME_LIB`, `sim-runtime-lib`, `build/sim/%` rule)
- Test: `build/sim/false_deadlock_test` under seeds 1 to 20 with the real runtime, all `pass`

**Interfaces:**
- Consumes: `include/goo_os.h` (Task 1), `sim.h` (Task 4).
- Produces: `build/libgoo_runtime_sim.a`; `build/sim/<program>` binaries; the environment contract `GOO_SIM_SEED`, `GOO_SIM_STEP_LIMIT`, `GOO_SIM_TIME_LIMIT_NS`, `GOO_SIM_TRACE_DIR`, `GOO_SIM_FAULT_SPURIOUS_WAKE` (0/1), `GOO_SIM_FAULT_THREAD_CREATE_EAGAIN` (0/1).

- [ ] **Step 1: `tests/sim/sim_os.c`.** Every seam function over `sim_sched.c`:

```c
/* Mutex ownership table: SIM_MUTEXES_MAX entries keyed by pointer. */
typedef struct { goo_mutex_t* mutex; uint32_t owner; uint8_t held; } sim_mutex_slot_t;
static sim_mutex_slot_t g_mutexes[SIM_MUTEXES_MAX];

goo_mutex_t* goo_mutex_new(void) { /* goo_alloc as production does; register a slot or SIM_LIMIT "mutexes" */ }
void goo_mutex_lock(goo_mutex_t* m) {
    sim_mutex_slot_t* s = slot_of(m);              /* asserts registered */
    GOO_ASSERT(!(s->held && s->owner == sim_current()->id));   /* negative space: no recursive lock */
    lock_order_check(m);                            /* SIM_INVARIANT "lock-order" if a channel mutex is taken after the scheduler mutex */
    while (s->held) { sim_block(SIM_WAIT_MUTEX, m); }
    s->held = 1; s->owner = sim_current()->id; push_held(m);
    sim_yield_point("lock");
}
void goo_mutex_unlock(goo_mutex_t* m) {
    sim_mutex_slot_t* s = slot_of(m);
    if (!s->held || s->owner != sim_current()->id) sim_exit_with(SIM_INVARIANT, "unlock-not-owner");
    s->held = 0; pop_held(m);
    sim_wake_one(m, SIM_WAIT_MUTEX);
    sim_yield_point("unlock");
}
int goo_mutex_try_lock(goo_mutex_t* m) { /* no wait; same checks; yield point either way */ }
void goo_cond_wait(goo_cond_t* c, goo_mutex_t* m) {
    sim_mutex_slot_t* s = slot_of(m);
    if (!s->held || s->owner != sim_current()->id) sim_exit_with(SIM_INVARIANT, "wait-without-mutex");
    goo_mutex_unlock_internal(m);                   /* no yield point of its own */
    if (fault_spurious_wake_on() && sim_random_below(8) == 0) { /* spurious: do not block */ }
    else { sim_block(SIM_WAIT_COND, c); }
    goo_mutex_lock(m);                              /* reacquire, with its yield point */
}
void goo_cond_signal(goo_cond_t* c)    { sim_wake_one(c, SIM_WAIT_COND); sim_yield_point("signal"); }
void goo_cond_broadcast(goo_cond_t* c) { sim_wake_all(c, SIM_WAIT_COND); sim_yield_point("broadcast"); }
goo_thread_t* goo_thread_create(goo_thread_func_t f, void* a) {
    if (fault_thread_create_eagain_on() && sim_random_below(16) == 0) return NULL;   /* the caller must handle it */
    uint32_t id = sim_thread_spawn(f, a); sim_yield_point("create"); return handle_for(id);
}
void goo_thread_join(goo_thread_t* t) { while (state(t) != SIM_EXITED) sim_block(SIM_WAIT_JOIN, t); }
void goo_platform_sleep_ns(uint64_t ns) { sim_sleep_until(sim_now_ns() + ns); }
uint64_t goo_platform_time_ns(void) { return sim_now_ns(); }
uint64_t goo_platform_wall_time_ns(void) { return sim_now_ns(); }
goo_worker_tls_t* goo_os_tls(void) { return &sim_current()->tls; }
```
Lock order: the scheduler mutex is `g_scheduler->scheduler_mutex`; the simulator learns it at the first `goo_mutex_lock` from a thread that already holds nothing and whose caller is `goo_scheduler_init` — simpler: `sim_os.c` exports `void sim_mark_scheduler_mutex(goo_mutex_t*)`, and `sim_main.c` calls it after `goo_scheduler_init` by reading `g_scheduler->scheduler_mutex` through the runtime's public accessor if one exists, else through an `extern` declared for the test build only. The check: if the current thread holds the scheduler mutex and takes any other, `SIM_INVARIANT "lock-order"`.

- [ ] **Step 2: `tests/sim/sim_main.c`.**

```c
extern int goo_sim_program_main(void);     /* the program's own main, renamed at compile time */
static void* program_thread(void* arg) { (void)arg; int rc = goo_sim_program_main(); if (rc != 0) sim_exit_with(SIM_PASS + 0, "program-nonzero"); /* see below */ return NULL; }
int main(void) {
    uint64_t seed = env_u64("GOO_SIM_SEED", 1);
    sim_init(seed, env_u64("GOO_SIM_STEP_LIMIT", 1000000), env_u64("GOO_SIM_TIME_LIMIT_NS", 10000000000ULL), getenv("GOO_SIM_TRACE_DIR"));
    sim_thread_spawn(program_thread, NULL);
    sim_run_until_done();                    /* switch into thread 0; returns when every thread has exited */
    return SIM_PASS;
}
```
A non-zero return from the program's main is its own verdict: exit with that value unchanged, so a stress program's "wrong count" exit stays visible. Write that as `_exit(rc)` after the trace dump.

- [ ] **Step 3: The build.** In the Makefile:

```make
SIM_RUNTIME_OBJS = $(filter-out $(BUILDDIR)/runtime/goo_os_posix.o $(BUILDDIR)/runtime/far_transport.o,$(RUNTIME_OBJS)) $(BUILDDIR)/sim/sim_sched.o $(BUILDDIR)/sim/sim_os.o
SIM_RUNTIME_LIB  = $(BUILDDIR)/libgoo_runtime_sim.a
$(BUILDDIR)/sim/%.o: tests/sim/%.c | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests/sim -c $< -o $@
$(SIM_RUNTIME_LIB): $(SIM_RUNTIME_OBJS) | $(BUILDDIR)
	rm -f $@ && ar -Drcs $@ $^
sim-runtime-lib: $(SIM_RUNTIME_LIB)
build/sim/%: tests/concurrency/%.c $(SIM_RUNTIME_LIB) $(BUILDDIR)/sim/sim_main.o
	@mkdir -p build/sim
	$(CC) -std=c23 -D_GNU_SOURCE -Iinclude -I. -Dmain=goo_sim_program_main $< $(BUILDDIR)/sim/sim_main.o $(SIM_RUNTIME_LIB) -lm -o $@
```
`far_transport.o` is excluded because it needs NNG and no program under test uses it; if the link then misses a symbol, add a stub file `tests/sim/sim_far_stub.c` with the missing functions asserting never-called, and record it.

- [ ] **Step 4: The negative control, first light.** `false_deadlock_test.c` requires `GOO_TEST_WAKE_DELAY_NS`; set it to 1 (the simulated sleep advances the virtual clock, which is exactly the hold-open the test wants).

```bash
make build/sim/false_deadlock_test; echo $?
for s in $(seq 1 20); do GOO_SIM_SEED=$s GOO_TEST_WAKE_DELAY_NS=1 GOO_SIM_TRACE_DIR=build/sim/trace ./build/sim/false_deadlock_test > build/sim/run.$s.log 2>&1; echo "seed=$s exit=$?"; done
```
Expected: exit 0 on all 20. Any other verdict is a finding: read the trace and report it before continuing. A `SIM_DEADLOCK` here most likely means a seam call the runtime makes that `sim_os.c` does not implement, or a wake path that never yields.

- [ ] **Step 5: Commit.** `feat(sim): the seam over the simulator, the process entry, and the sim archive`.

---

### Task 6: The driver, replay, the self-test, and the positive control

**Files:**
- Create: `scripts/sim_probe.sh`, `tests/sim/sim_invariant_test.c`
- Modify: `Makefile` (`build/sim/sim_invariant_test` uses the same rule from `tests/sim/`; add a second pattern rule for `tests/sim/%.c`)
- Test: `scripts/sim_probe.sh false_deadlock_test --seeds 1000` and `--self-test`

- [ ] **Step 1: The driver**

```bash
#!/usr/bin/env bash
# sim_probe.sh — run one program under the simulator over N seeds, one process
# per seed, replay every non-pass, and report by verdict.
# Usage: scripts/sim_probe.sh <program> --seeds N [--first-seed K] [--fault-spurious-wake] [--fault-thread-create]
#        scripts/sim_probe.sh --self-test
set -u
PROBE="sim-probe"
ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$ROOT" || exit 2
verdict_name() { case "$1" in 0) echo pass;; 2) echo runtime-deadlock-abort;; 10) echo sim-deadlock;; 11) echo invariant;; 12) echo step-limit;; 13) echo time-limit;; 14) echo replay-mismatch;; 15) echo limit;; *) echo "crash-$1";; esac; }
run_seed() {   # $1 bin, $2 seed, $3 tracedir -> prints "seed verdict steps" ; returns exit
    local out; out="$(GOO_SIM_SEED="$2" GOO_SIM_TRACE_DIR="$3" GOO_TEST_WAKE_DELAY_NS="${GOO_TEST_WAKE_DELAY_NS:-1}" "$1" 2>"$3/$2.err")"; local rc=$?
    echo "$2 $(verdict_name $rc) $(grep -o 'steps=[0-9]*' "$3/$2.err" | tail -1)"; return $rc
}
replay_matches() {   # $1 bin, $2 seed, $3 tracedir -> 0 if the second run's trace equals the first
    cp "$3/$2.trace" "$3/$2.trace.first"
    run_seed "$1" "$2" "$3" >/dev/null 2>&1
    cmp -s "$3/$2.trace.first" "$3/$2.trace"
}
```
Then the main loop: build `build/sim/<program>` via `make`, loop seeds, one line per seed, a summary `verdict count` table, and for each non-pass seed the replay check; if a replay differs, the summary line for that seed becomes `replay-mismatch` and the probe exits 14 regardless of anything else. Exit 0 only if every seed passed. The verdict line names the artefact: `sim-probe: PASS (build/sim/<program>, seeds K..K+N-1, steps total S, trace dir D)`.

`--self-test`: (a) build a mutant sim archive in a scratch dir: copy `src/runtime/concurrency.c`, apply the same `sed` `scripts/false_deadlock_probe.sh:136` applies to `goo_sched_wake_waiters`, compile it against the other sim objects, link `false_deadlock_test`, run seeds 1..256; expect at least one `runtime-deadlock-abort` and a byte-identical replay of the first such seed; record `seeds_to_first_failure`. (b) build `build/sim/sim_invariant_test` (a program whose main locks a mutex on one thread and unlocks it on another) and expect `invariant` on seed 1. (c) a control: the unmutated `false_deadlock_test` on seeds 1..32 all `pass`. Print `sim-probe --self-test: PASS (mutant red at seed <k>, invariant red, control green)`.

- [ ] **Step 2: The positive control, measured.** Run the self-test and record: the seed of first failure, the steps, the wall time per seed (`/usr/bin/time -f %e` around the loop divided by seeds). Then run `scripts/sim_probe.sh false_deadlock_test --seeds 1000` on the real runtime: expect PASS. These two numbers are the spike's answer for section 3 of the spec.

- [ ] **Step 3: Commit.** `feat(sim): the seed driver with replay, and the false-deadlock positive control`.

---

### Task 7: The other four programs, the README, and the report

**Files:**
- Create: `tests/sim/README.md`
- Test: `scripts/sim_probe.sh <p> --seeds 1000` for `deadlock_poll_test`, `mt_scheduler_stress`, `chan_mt_stress`, `fanin_stress`

- [ ] **Step 1: Run each program.** `deadlock_poll_test` expects `runtime-deadlock-abort` on every seed (it is a real deadlock by construction): the driver needs `--expect runtime-deadlock-abort` so that verdict counts as pass for that program; add the flag. The three stress programs expect `pass`. Record per program: verdict table, seeds run, wall time per seed, max steps, whether any limit was hit. A non-pass on a stress program is a finding: replay it, read the trace, and report it with the trace path; do not tune limits to make it pass.

- [ ] **Step 2: Write `tests/sim/README.md`:** how to build and run one seed by hand, how to read a trace line, how to replay, the environment contract, the verdict table, the limits, and the spike's numbers table (program, seeds, verdicts, wall time per seed, T3 nesting line from Task 4).

- [ ] **Step 3: Commit.** `docs(sim): how to run, replay and read the simulator, with the spike's numbers`.

---

### Task 8: The decision, and the gate if the answer is yes

**Files:**
- Modify: `Makefile` (`sim-false-deadlock-probe`, `sim-false-deadlock-selftest` in `VERIFY_ALL_DEPS`), `tools/parity-gate-count.txt` (+2), `docs/superpowers/plans/2026-09-02-verification-tooling-next-steps.md` (a line in "Out of scope" or a new track pointing here), `docs/superpowers/specs/2026-09-03-deterministic-simulation-design.md` (a "Result" section with the numbers and the decision)

- [ ] **Step 1: Decide from Task 6 and 7's numbers**, against section 3 of the spec: positive control red within 256 seeds with replay; negative control green over 1,000; no stress-program finding left open. Write the result section into the spec with the numbers and the date.

- [ ] **Step 2: If yes,** add:
```make
sim-false-deadlock-probe: $(SIM_RUNTIME_LIB)
	@bash scripts/sim_probe.sh false_deadlock_test --seeds 256
sim-false-deadlock-selftest: $(SIM_RUNTIME_LIB)
	@bash scripts/sim_probe.sh --self-test
```
both in `VERIFY_ALL_DEPS`, parity 229 → 231, and measure the wall time of both inside `make verify-core` (target: about 20 s together; if more, lower the gate's seed count and record why). `./scripts/probe_teeth_probe.sh` PASS; parity PASS.

- [ ] **Step 3: If no,** write why in the spec's result section with the trace that decided it; the seam, the gate from Task 3, and the simulator stay; the fallback (approach B) becomes a new spec.

- [ ] **Step 4: Commit.** `feat(probes): sim-false-deadlock-probe joins verify-core, with the three companion edits` or `docs(spec): the simulation spike's result`.

---

## Self-review against the spec

- Section 2 scope: Tasks 1-7 touch only the scheduler, channels and sync; no time API, no far transport (excluded from the sim archive).
- Section 3 pass bar: Task 6 Step 2 (positive control, replay) and Task 7 Step 1 (negative controls); Task 8 decides.
- Section 5 seam: Task 1-2 build it; Task 3 guards it; `goo_os_tls()` in Task 2.
- Section 6 simulator: Task 4; limits and verdicts as constants in `sim.h`.
- Section 7 checks and faults: Task 5 (`lock-order`, `unlock-not-owner`, `wait-without-mutex`; the two faults behind env flags, off by default); Tiger Style rules in Global Constraints.
- Section 8 harness: Task 5 (build, `-Dmain`), Task 6 (driver, replay, self-test with the wake-site mutant and the invariant program).
- Section 9 steps: Task 1-3 = step 1; Task 4 T3 = step 2; Task 5-6 = step 3; Task 7 = step 4; Task 8 = step 5.
- Names used across tasks: `sim_yield_point`, `sim_block`, `sim_wake_one`, `sim_wake_all`, `sim_sleep_until`, `sim_now_ns`, `sim_random_below`, `sim_exit_with`, `sim_thread_spawn`, `sim_run_until_done`, `goo_os_tls`, `goo_cond_*`, `goo_thread_create/join`, `goo_sim_program_main` — each defined once (Task 1 or 4) and used with the same signature later.
