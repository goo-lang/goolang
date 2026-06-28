# Scheduler Default-Flip (Parallelism-by-Default) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Flip Goo's goroutine scheduler from single-threaded-by-default to NCPU/`GOMAXPROCS` parallelism-by-default, Go-faithfully, with looped soak probes proving codegen-emitted goroutine programs are race-free under real parallelism.

**Architecture:** A new side-effect-free resolver `goo_default_thread_count()` computes the worker count (GOMAXPROCS env if valid `>=1`, else online CPU count, clamped to `[1, 16]`). The single lazy-init call site in `goo_go` switches from `goo_scheduler_init(1)` to `goo_scheduler_init(goo_default_thread_count())`. `goo_scheduler_init` itself and the deadlock detector are unchanged. Two looped `.goo` soak probes (fan-in + select-heavy) and a resolver unit test gate the change; CI pins `GOMAXPROCS=4` for a reproducible multi-threaded run of the whole suite.

**Tech Stack:** C23 runtime (`src/runtime/concurrency.c`), POSIX `pthread`/`sysconf`/`getenv`/`strtol`, the Goo compiler (`bin/goo`), GNU Make probe gates, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-06-29-scheduler-default-flip-design.md`

## Global Constraints

- Thread-count cap: `GOO_MAX_OS_THREADS = 16` (unchanged). Every resolved count is clamped to `[1, 16]`.
- `goo_scheduler_init(int)` signature is unchanged; it already normalizes `<= 0 -> 1` and clamps to the cap. Only the *argument* passed at the lazy-init site changes.
- The deadlock detector (`goo_scheduler_wait` stability gate, 3 x 0.5ms) must not be modified.
- Explicit-init callers (`tests/concurrency/*_stress.c` calling `goo_scheduler_init(4)`) must remain unaffected.
- Default thread count: GOMAXPROCS env if it parses to an integer `>= 1` (honored even above NCPU, Go-faithful), else `sysconf(_SC_NPROCESSORS_ONLN)`; invalid/`<1`/non-numeric falls back to NCPU; `sysconf` failure floors to 1.
- Soak probes loop `PARALLEL_SOAK_ITERS ?= 50` times; **any** iteration with wrong stdout or nonzero exit fails the probe.
- Build/recipe pattern for C tests mirrors the stress tests exactly: `$(CC) -std=c23 -D_GNU_SOURCE -Iinclude -I. <test.c> $(RUNTIME_LIB) -lpthread -lm -o build/<name>` where `CC = gcc`, `$(RUNTIME_LIB) = lib/libgoo_runtime.a`, `$(COMPILER) = bin/goo`.

**Design refinement (vs spec §3):** the resolver uses raw `getenv("GOMAXPROCS")` rather than `goo_os_getenv`. Rationale: we are in internal runtime C, `getenv` returns a non-owned pointer (no allocation to free, unlike `goo_os_getenv` which `strdup`s), and `<stdlib.h>`/`<unistd.h>` are already included in `concurrency.c`. Behavior is identical to the spec's intent.

---

### Task 1: `goo_default_thread_count()` resolver + unit test

**Files:**
- Create: `tests/concurrency/default_thread_count_test.c`
- Modify: `src/runtime/concurrency.c` (add the function; near the other scheduler functions, e.g. just above `goo_scheduler_init` at line ~59)
- Modify: `include/runtime.h` (declare the prototype near `goo_scheduler_init`, line ~169)
- Modify: `Makefile` (add `default-thread-count-test` target)

**Interfaces:**
- Produces: `int goo_default_thread_count(void);` — external linkage, side-effect-free. Returns the resolved worker count in `[1, 16]`.

- [ ] **Step 1: Write the failing test**

Create `tests/concurrency/default_thread_count_test.c`:

```c
// Unit test for goo_default_thread_count(): GOMAXPROCS/NCPU resolution policy.
// Pure function, no threads — deterministic. Mirrors the spec's resolution table.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// Resolver under test (external linkage in src/runtime/concurrency.c).
extern int goo_default_thread_count(void);

#define CAP 16

// Expected NCPU-path result: online CPU count, floored to 1, clamped to CAP.
static int expected_ncpu(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > CAP) n = CAP;
    return (int)n;
}

static int fails = 0;
static void check(const char* name, int got, int want) {
    if (got != want) { printf("FAIL %s: got %d want %d\n", name, got, want); fails++; }
    else { printf("ok %s = %d\n", name, got); }
}

int main(void) {
    setenv("GOMAXPROCS", "1", 1);
    check("gomaxprocs=1", goo_default_thread_count(), 1);

    setenv("GOMAXPROCS", "8", 1);          // honored even if NCPU < 8 (Go-faithful)
    check("gomaxprocs=8", goo_default_thread_count(), 8);

    setenv("GOMAXPROCS", "99", 1);         // above cap -> clamp to 16
    check("gomaxprocs=99->clamp", goo_default_thread_count(), CAP);

    setenv("GOMAXPROCS", "abc", 1);        // non-numeric -> NCPU
    check("gomaxprocs=garbage->ncpu", goo_default_thread_count(), expected_ncpu());

    setenv("GOMAXPROCS", "0", 1);          // < 1 -> NCPU
    check("gomaxprocs=0->ncpu", goo_default_thread_count(), expected_ncpu());

    unsetenv("GOMAXPROCS");                 // unset -> NCPU
    check("unset->ncpu", goo_default_thread_count(), expected_ncpu());

    if (fails) { printf("default-thread-count-test: FAIL (%d)\n", fails); return 1; }
    printf("default-thread-count-test: PASS\n");
    return 0;
}
```

Add the `Makefile` target (place it near `mt-scheduler-stress`):

```make
# Default-thread-count resolver: GOMAXPROCS/NCPU policy, clamped to [1,16].
default-thread-count-test: $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== default-thread-count-test: GOMAXPROCS/NCPU resolution ==="
	$(CC) -std=c23 -D_GNU_SOURCE -Iinclude -I. tests/concurrency/default_thread_count_test.c $(RUNTIME_LIB) -lpthread -lm -o build/default_thread_count_test
	@timeout 10 ./build/default_thread_count_test; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "default-thread-count-test: PASS"; else echo "default-thread-count-test: FAIL (exit $$rc)"; exit 1; fi
```

- [ ] **Step 2: Run test to verify it fails**

Run: `eval $(opam env --switch=default) && make default-thread-count-test`
Expected: FAIL — link error `undefined reference to 'goo_default_thread_count'` (function not yet defined).

- [ ] **Step 3: Write minimal implementation**

In `src/runtime/concurrency.c`, add the function just above `goo_scheduler_init` (line ~59). `<stdlib.h>` (for `getenv`/`strtol`) and `<unistd.h>` (for `sysconf`) are already included at the top of the file.

```c
// Resolve the default OS-thread count for lazy scheduler init.
// Policy (Go-faithful): GOMAXPROCS env if it parses to an integer >= 1 (honored
// even above NCPU), else the online CPU count; clamped to [1, GOO_MAX_OS_THREADS].
// Side-effect-free. Exposed (non-static) so the resolver is unit-testable.
int goo_default_thread_count(void) {
    const char* env = getenv("GOMAXPROCS");
    if (env && env[0] != '\0') {
        char* end = NULL;
        long n = strtol(env, &end, 10);
        // Whole string must be a valid integer >= 1; otherwise fall through.
        if (end != env && *end == '\0' && n >= 1) {
            return (n > GOO_MAX_OS_THREADS) ? GOO_MAX_OS_THREADS : (int)n;
        }
        // invalid / < 1 -> NCPU (Go ignores an invalid GOMAXPROCS)
    }
#ifdef GOO_PLATFORM_UNIX
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) ncpu = 1;                 // sysconf failure -> safe floor
    return (ncpu > GOO_MAX_OS_THREADS) ? GOO_MAX_OS_THREADS : (int)ncpu;
#else
    return 1;
#endif
}
```

In `include/runtime.h`, add the prototype next to `goo_scheduler_init` (line ~169):

```c
int goo_default_thread_count(void);
```

- [ ] **Step 4: Run test to verify it passes**

Run: `eval $(opam env --switch=default) && make default-thread-count-test`
Expected: every `ok ...` line prints, final `default-thread-count-test: PASS`.

- [ ] **Step 5: Commit**

```bash
git add tests/concurrency/default_thread_count_test.c src/runtime/concurrency.c include/runtime.h Makefile
git commit --no-gpg-sign -m "feat(runtime): GOMAXPROCS/NCPU default-thread-count resolver + unit test"
```

---

### Task 2: Flip the lazy-init call site

**Files:**
- Modify: `src/runtime/concurrency.c:175` (inside `goo_go`)

**Interfaces:**
- Consumes: `goo_default_thread_count()` from Task 1.

- [ ] **Step 1: Write the failing test**

No new test file — the gate is the existing concurrency probe suite, which after this change runs multi-threaded by default. First confirm the pre-change baseline still describes single-thread init. Run:

`grep -n "goo_scheduler_init(1)" src/runtime/concurrency.c`
Expected (pre-change): one hit at line ~175.

- [ ] **Step 2: Verify current behavior**

Run: `eval $(opam env --switch=default) && make go-probe unbuffered-probe select-probe`
Expected: all PASS (they pass single-threaded today; this captures the green baseline before the flip).

- [ ] **Step 3: Make the change**

In `src/runtime/concurrency.c`, inside `goo_go` (line ~173-176), replace the lazy-init line:

```c
goo_goroutine_t* goo_go(goo_goroutine_func_t func, void* arg) {
    if (!g_scheduler) {
        goo_scheduler_init(goo_default_thread_count());  // was: goo_scheduler_init(1)
    }
```

- [ ] **Step 4: Run the concurrency suite to verify it still passes (now multi-threaded)**

Run: `eval $(opam env --switch=default) && make go-probe unbuffered-probe select-probe chan-probe chan-elem-probe chan-padded-probe chan-uint-probe block-scope-probe escape-probe escape-range-probe deadlock-probe deadlock-goroutine-probe`
Expected: every probe `PASS`. (On a multi-core box these now exercise N>1 workers. A failure here is a real latent race the flip exposed — stop and debug with systematic-debugging, do not paper over it.)

- [ ] **Step 5: Commit**

```bash
git add src/runtime/concurrency.c
git commit --no-gpg-sign -m "feat(runtime): flip goroutine scheduler default to NCPU/GOMAXPROCS parallelism"
```

---

### Task 3: Fan-in soak probe

**Files:**
- Create: `examples/parallel_soak_probe.goo`
- Modify: `Makefile` (add `parallel-soak-probe` target + `PARALLEL_SOAK_ITERS` var)

**Interfaces:**
- Consumes: the multi-threaded default from Task 2.

- [ ] **Step 1: Write the soak program**

Create `examples/parallel_soak_probe.goo`:

```goo
// parallel-soak-probe: 64 goroutines fan-in to one shared buffered channel;
// main sums 64 receives. The sum is 64 regardless of scheduling interleaving.
// Looped many times under the default multi-threaded scheduler to hunt
// nondeterministic races in the codegen-emitted goroutine + channel path.
package main

import "fmt"

func worker(done chan int) {
    done <- 1
}

func main() {
    done := make_chan(int, 64)
    for i := 0; i < 64; i = i + 1 {
        go worker(done)
    }
    sum := 0
    for i := 0; i < 64; i = i + 1 {
        sum = sum + <-done
    }
    fmt.Println(sum)
}
```

Add the `Makefile` target (near the other probes; define `PARALLEL_SOAK_ITERS` once, reused by Task 4):

```make
# Soak iteration count for the parallel probes (override: make ... PARALLEL_SOAK_ITERS=200).
PARALLEL_SOAK_ITERS ?= 50

# parallel-soak-probe: 64-goroutine channel fan-in, deterministic sum=64,
# run PARALLEL_SOAK_ITERS times under the default multi-threaded scheduler.
parallel-soak-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== parallel-soak-probe: 64-goroutine fan-in x $(PARALLEL_SOAK_ITERS) (default parallelism) ==="
	$(COMPILER) -o build/parallel_soak_probe examples/parallel_soak_probe.goo
	@for i in $$(seq 1 $(PARALLEL_SOAK_ITERS)); do \
	  out=$$(timeout 10 ./build/parallel_soak_probe); rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "parallel-soak-probe: FAIL (iter $$i exit $$rc)"; exit 1; fi; \
	  if [ "$$out" != "64" ]; then echo "parallel-soak-probe: FAIL (iter $$i got '$$out' want 64)"; exit 1; fi; \
	done; \
	echo "parallel-soak-probe: PASS ($(PARALLEL_SOAK_ITERS) iters, sum=64)"
```

- [ ] **Step 2: Run to verify it passes**

Run: `eval $(opam env --switch=default) && make parallel-soak-probe`
Expected: `parallel-soak-probe: PASS (50 iters, sum=64)`.

- [ ] **Step 3: Stress it to confirm determinism under high iteration**

Run: `eval $(opam env --switch=default) && make parallel-soak-probe PARALLEL_SOAK_ITERS=200`
Expected: `parallel-soak-probe: PASS (200 iters, sum=64)`. (If any iteration prints a value other than 64 or exits nonzero, that is a real race — stop and debug.)

- [ ] **Step 4: Commit**

```bash
git add examples/parallel_soak_probe.goo Makefile
git commit --no-gpg-sign -m "test(runtime): parallel fan-in soak probe (64 goroutines, looped)"
```

---

### Task 4: Select-heavy soak probe

**Files:**
- Create: `examples/parallel_select_soak_probe.goo`
- Modify: `Makefile` (add `parallel-select-soak-probe` target; reuses `PARALLEL_SOAK_ITERS` from Task 3)

**Interfaces:**
- Consumes: the multi-threaded default from Task 2; `PARALLEL_SOAK_ITERS` from Task 3.

- [ ] **Step 1: Write the soak program**

Create `examples/parallel_select_soak_probe.goo`:

```goo
// parallel-select-soak-probe: 32 goroutines send on channel a, 32 on channel b;
// main performs 64 blocking selects, counting each receive. The count is 64
// regardless of interleaving. Exercises the select wakeup path (distinct from
// fan-in) under contention across the default multi-threaded scheduler.
package main

import "fmt"

func send_a(a chan int) {
    a <- 1
}

func send_b(b chan int) {
    b <- 1
}

func main() {
    a := make_chan(int, 64)
    b := make_chan(int, 64)
    for i := 0; i < 32; i = i + 1 {
        go send_a(a)
    }
    for i := 0; i < 32; i = i + 1 {
        go send_b(b)
    }
    count := 0
    for i := 0; i < 64; i = i + 1 {
        select {
        case <-a:
            count = count + 1
        case <-b:
            count = count + 1
        }
    }
    fmt.Println(count)
}
```

Add the `Makefile` target:

```make
# parallel-select-soak-probe: 32+32 goroutines feeding two channels; main runs
# 64 blocking selects, deterministic count=64, looped PARALLEL_SOAK_ITERS times.
parallel-select-soak-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== parallel-select-soak-probe: 64 selects over 2 channels x $(PARALLEL_SOAK_ITERS) (default parallelism) ==="
	$(COMPILER) -o build/parallel_select_soak_probe examples/parallel_select_soak_probe.goo
	@for i in $$(seq 1 $(PARALLEL_SOAK_ITERS)); do \
	  out=$$(timeout 10 ./build/parallel_select_soak_probe); rc=$$?; \
	  if [ $$rc -ne 0 ]; then echo "parallel-select-soak-probe: FAIL (iter $$i exit $$rc)"; exit 1; fi; \
	  if [ "$$out" != "64" ]; then echo "parallel-select-soak-probe: FAIL (iter $$i got '$$out' want 64)"; exit 1; fi; \
	done; \
	echo "parallel-select-soak-probe: PASS ($(PARALLEL_SOAK_ITERS) iters, count=64)"
```

- [ ] **Step 2: Run to verify it passes**

Run: `eval $(opam env --switch=default) && make parallel-select-soak-probe`
Expected: `parallel-select-soak-probe: PASS (50 iters, count=64)`.

- [ ] **Step 3: Stress it at high iteration**

Run: `eval $(opam env --switch=default) && make parallel-select-soak-probe PARALLEL_SOAK_ITERS=200`
Expected: `parallel-select-soak-probe: PASS (200 iters, count=64)`.

- [ ] **Step 4: Commit**

```bash
git add examples/parallel_select_soak_probe.goo Makefile
git commit --no-gpg-sign -m "test(runtime): parallel select-heavy soak probe (32+32 goroutines, looped)"
```

---

### Task 5: Wire into `verify:` and `tests.yml` (pin CI to GOMAXPROCS=4)

**Files:**
- Modify: `Makefile:862` (the `verify:` dependency list)
- Modify: `.github/workflows/tests.yml` (the "Language probes" step, lines ~51-54)

**Interfaces:**
- Consumes: `default-thread-count-test` (Task 1), `parallel-soak-probe` (Task 3), `parallel-select-soak-probe` (Task 4).

- [ ] **Step 1: Append the new gates to `verify:`**

In `Makefile`, at the end of the `verify:` dependency list (line 862, currently ending `... deadlock-probe deadlock-goroutine-probe`), append:

```
 default-thread-count-test parallel-soak-probe parallel-select-soak-probe
```

So the line ends: `... deadlock-probe deadlock-goroutine-probe default-thread-count-test parallel-soak-probe parallel-select-soak-probe`

- [ ] **Step 2: Wire the CI probe step + pin GOMAXPROCS=4**

In `.github/workflows/tests.yml`, the "Language probes" step (the `run: |` block at lines ~51-54), make two edits:

(a) Add the pin as the first line of the script body (just after `run: |`, before the `LLVMCFG=...` line), so every probe in this step runs at a fixed 4 workers:

```yaml
        run: |
          export GOMAXPROCS=4
          LLVMCFG="$(ls /usr/bin/llvm-config-* 2>/dev/null | sort -V | tail -1)"
```

(b) Append the three new targets to the end of the `make ...` target list (the line currently ending `... deadlock-probe deadlock-goroutine-probe`):

```
 default-thread-count-test parallel-soak-probe parallel-select-soak-probe
```

> Note: `default-thread-count-test` sets/unsets `GOMAXPROCS` internally via `setenv`, so the exported pin does not affect its assertions; the soak and language probes honor the pinned `GOMAXPROCS=4`.

- [ ] **Step 3: Run the full local gate**

Run: `eval $(opam env --switch=default) && make verify`
Expected: ends with `verify: ALL GREEN GATES PASSED`, including `default-thread-count-test: PASS`, `parallel-soak-probe: PASS`, `parallel-select-soak-probe: PASS`.

- [ ] **Step 4: Run the unit-test suite (no regressions)**

Run: `eval $(opam env --switch=default) && make test 2>&1 | grep -E "Passed:|Failed:|Skipped:|All tests"`
Expected: `Passed: 76`, `Skipped: 1`, `All tests passed!`.

- [ ] **Step 5: Reproduce the CI pin locally (sanity-check the fixed-N path)**

Run: `eval $(opam env --switch=default) && GOMAXPROCS=4 make parallel-soak-probe parallel-select-soak-probe`
Expected: both `PASS`. (Confirms the pinned-N=4 configuration CI uses is green.)

- [ ] **Step 6: Commit**

```bash
git add Makefile .github/workflows/tests.yml
git commit --no-gpg-sign -m "ci(runtime): gate default-thread-count + soak probes; pin GOMAXPROCS=4 in CI"
```

---

## Final verification

- [ ] `goo_default_thread_count()` honors `GOMAXPROCS` (`1`, `8`, `99->16`) and falls back to NCPU on `0`/garbage/unset — `default-thread-count-test: PASS`.
- [ ] Lazy init spawns NCPU/`GOMAXPROCS` workers (call site changed; `goo_scheduler_init` signature and deadlock detector untouched).
- [ ] Whole `verify:` suite green under the multi-threaded default; `make test` 76/1.
- [ ] Both soak probes green across 50 iters locally and 200-iter stress (no wrong output, no nonzero exit, no false deadlock abort).
- [ ] CI step pins `GOMAXPROCS=4`; the three new gates wired into `verify:` and `tests.yml`.
- [ ] Explicit-init C stress tests (`mt-scheduler-stress`, `chan-mt-stress`, `yield-stress`) still green (bypass the lazy path; unaffected).

## Spec coverage self-check

| Spec element | Task |
|---|---|
| Resolver: GOMAXPROCS-or-NCPU, clamped [1,16], named function | 1 |
| Resolver edge cases (sysconf<1, GOMAXPROCS<=0/garbage) + unit test | 1 |
| Flip lazy `init(1)` -> resolver | 2 |
| `goo_scheduler_init` & deadlock detector unchanged | 2 (constraint) |
| Fan-in soak probe (looped, deterministic) | 3 |
| Select-heavy soak probe (looped, deterministic) | 4 |
| Whole-suite-goes-parallel regression gate | 5 (verify under default) |
| CI pin GOMAXPROCS=4 + wire all three gates | 5 |
| Explicit-init stress tests unaffected | 2, 5 (final verification) |
