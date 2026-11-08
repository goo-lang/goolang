# Deterministic simulation of the runtime scheduler — design

Status: approved in conversation on 2026-09-03, section by section; written for
review before the implementation plan. Companion to
`docs/superpowers/specs/2026-06-29-m9-deadlock-detection-design.md` (the
protocol) and `tests/models/deadlock_detector.pml` (the SPIN proof of it).

## 1. Purpose

The SPIN model proves the deadlock-detector protocol. Nothing checks the CODE
that implements it under a chosen interleaving, replayable from a seed. The
stress gates (`mt-scheduler-stress`, `chan-mt-stress`, `fanin-stress`,
`yield-stress`) run wall-clock and nondeterministic: a failure they find
cannot be replayed, and a pass says only that this machine, today, did not
hit the bad interleaving. This design gives the runtime a simulator in the
shape of TigerBeetle's and FoundationDB's: one real thread, a seeded
scheduler at every synchronisation point, a virtual clock, a trace, and a
replay that must reproduce the trace byte for byte.

The first deliverable is a spike with a pass bar (section 3). The seam it
introduces (section 4) stays whatever the answer.

## 2. Scope

In: the scheduler (`src/runtime/concurrency.c`) and channels
(`src/runtime/channels.c`): goroutines, send, receive, close, select, the
deadlock detector, the poll loop. The detector's own `goo_platform_sleep_ns`
poll is the only time the first cut simulates.

Out, for later cuts: the user-facing time APIs (timeouts, `time.After`,
sleep) under the virtual clock; the far transport (NNG lanes) and its
network fault model; memory-model litmus tests for the runtime's one atomic
(a `herd7` item for the fault-model document).

## 3. The pass bar for the spike

The spike passes when all three hold:

1. **Positive control.** The runtime with the task 8 correction reverted (the
   wake-site mutant that `scripts/false_deadlock_probe.sh --self-test`
   already produces) reaches the verdict `runtime-deadlock-abort` on
   `tests/concurrency/false_deadlock_test.c` within the seed budget. That is
   the false deadlock, a real defect, found from a seed.
2. **Negative control.** The corrected runtime reaches `pass` on every seed
   of the same budget, for the same program.
3. **Replay.** The failing seed of control 1, run again, produces a trace
   byte-identical to the first. The harness asserts this on every non-pass
   verdict, always.

Numbers recorded by the spike: seeds to first failure, wall time per seed,
the verdict table for every program, and which limits were hit.

## 4. Decision record

**Approach A over B and C.** A: cooperative simulated threads on one real
thread behind an explicit seam (chosen). B: real threads under a scheduling
token; rejected because any OS call outside the seam runs untokened and
breaks determinism with no error. C: chaos scheduling plus `rr` replay;
rejected as the spike because replay then needs `rr` on the machine, so CI
cannot replay and a seed does not reproduce across machines. C is a possible
complement later.

**The runtime changes through one seam, nothing else.** The alternative,
link-time symbol substitution on an unmodified runtime, was rejected because
the seam is then invisible and every new `pthread_` call escapes the
simulator silently. A grep gate makes the seam visible and keeps it whole.

**Nested contexts are the technical risk, measured first.** Goroutines use
`swapcontext` inside a worker; under the simulator the worker is itself a
coroutine. A seam call from inside a goroutine saves the goroutine's stack
state and resumes there; the worker's own saved context is untouched. If the
measurement in step 2 of section 9 fails, the fallback is B with the same
seam.

## 5. The seam

Header `include/goo_os.h`. Two implementations that never link together:
`src/runtime/goo_os_posix.c` (production) and `tests/sim/sim_os.c` (the
simulator). Existing seam calls stay: `goo_mutex_lock`, `goo_mutex_unlock`,
`goo_mutex_try_lock`, `goo_platform_sleep_ns`.

| Seam call | Replaces | Simulator behaviour |
|---|---|---|
| `goo_cond_init`, `goo_cond_destroy` | `pthread_cond_init`, `pthread_cond_destroy` | a fixed waiter list |
| `goo_cond_wait(cond, mutex)` | `pthread_cond_wait` | park; release the mutex; on wake reacquire it; the seed may return spuriously when that fault is on |
| `goo_cond_signal`, `goo_cond_broadcast` | the pthread pair | one waiter (seed picks which) or all waiters become runnable |
| `goo_thread_create`, `goo_thread_join` | `pthread_create`, `pthread_join` | a new coroutine with its own stack; join parks until exit; create may fail with `EAGAIN` when that fault is on |
| `goo_mutex_lock`, `goo_mutex_unlock`, `goo_mutex_try_lock` | already seam calls | scheduling point; owner, double-unlock and lock-order checks |
| `goo_platform_sleep_ns` | already a seam call | park with a deadline on the virtual clock |
| `goo_os_tls()` | the four `_Thread_local` variables in `concurrency.c` (`t_sched_ctx`, `t_current`, `t_reap`, `t_requeue`) | the running coroutine's own copy of the struct |

Rules:
- After the change, `src/runtime/concurrency.c` and `src/runtime/channels.c`
  contain no `pthread_` token and no `_Thread_local`. A verify-core gate,
  `runtime-seam-probe`, greps for both with a positive control (a scratch
  copy with one `pthread_mutex_lock` injected must go red).
- Every seam call is a scheduling point: the simulator may switch coroutines
  there and nowhere else.
- The one `_Atomic` in `concurrency.c` stays C11 atomics. One real thread
  needs nothing from it.
- The runtime's logic does not change. The commit that introduces the seam
  is proven behaviour-neutral: every concurrency gate, the stress gates and
  verify-core green before and after, and the production archive's object
  list unchanged except for the added `goo_os_posix.o`.

## 6. The simulator

Files: `tests/sim/sim_sched.c` (scheduler, clock, trace), `tests/sim/sim_os.c`
(the seam), `tests/sim/sim_main.c` (process entry), `tests/sim/sim.h`.

**Coroutines.** Each simulated thread is a `ucontext_t` with its own stack.
The program's main thread is coroutine 0.

**Scheduling points.** Every seam call ends in `sim_yield_point()`. The
scheduler picks the next runnable coroutine uniformly with `xorshift64*`
from the seed. Between seam calls only straight-line code runs, so the seed
fully determines the interleaving.

**Blocking.** `goo_cond_wait` marks the coroutine waiting on that condvar and
switches away; signal moves one waiter (seed-chosen) to runnable; broadcast
moves all; join waits on thread exit; lock on a held mutex waits on the
mutex.

**Virtual clock.** `goo_platform_sleep_ns` parks the coroutine with a
deadline. When nothing is runnable, the clock jumps to the earliest deadline
and wakes those sleepers. No run waits in real time. Nothing runnable and no
sleeper is the verdict `sim-deadlock`, distinct from the runtime's own abort.

**Trace.** Every scheduling decision appends one fixed-size record to a ring:
step, virtual time, thread, seam call, choice. On any non-pass exit the ring
is written to `$GOO_SIM_TRACE_DIR/<seed>.trace` by an `atexit` handler and
by the abort-signal handler.

**Limits, all compile-time constants, all asserted at the seam, each with a
distinct verdict when exceeded:**

| Constant | Value | Reason |
|---|---|---|
| `SIM_THREADS_MAX` | 17 | `GOO_MAX_OS_THREADS` (16) workers plus main |
| `SIM_STACK_BYTES` | 262,144 | one worker's scheduler frames; goroutine stacks are the runtime's own |
| `SIM_WAITERS_MAX` | 17 | one entry per thread per condvar |
| `SIM_MUTEXES_MAX` | 64 | scheduler mutex plus channel mutexes in the programs under test |
| `SIM_TRACE_ENTRIES` | 65,536 | 32 bytes each, 2 MiB; older entries roll off, the count is kept |
| `GOO_SIM_STEP_LIMIT` (env) | default 1,000,000 | the run bound |
| `GOO_SIM_TIME_LIMIT_NS` (env) | default 10 s virtual | the clock bound |

## 7. Checks, faults, verdicts, and the simulator's own rules

**Invariants checked at every seam call**, each an immediate `invariant`
verdict with the trace: unlock by a non-owner; double unlock; wait without
the mutex held; lock order (channel mutex before scheduler mutex, the order
the task 8 reviews established at all 12 sites); exit while holding a mutex;
join on a thread that never exits.

**Faults, each behind a harness flag, off by default** (so both controls of
section 3 run with all faults off): a spurious return from `goo_cond_wait`;
`goo_thread_create` failing with `EAGAIN`. Both are behaviours POSIX permits.
Lost signals are out: POSIX does not permit them.

**Verdicts and exit codes** (the runtime's own codes pass through):

| Verdict | Exit | Meaning |
|---|---|---|
| `pass` | 0 | every coroutine exited, program's own checks passed |
| `runtime-deadlock-abort` | 2 | the runtime's `goo_deadlock_abort` |
| `sim-deadlock` | 10 | nothing runnable, nothing sleeping |
| `invariant` | 11 | a section 7 check failed |
| `step-limit` | 12 | `GOO_SIM_STEP_LIMIT` reached |
| `time-limit` | 13 | `GOO_SIM_TIME_LIMIT_NS` reached |
| `replay-mismatch` | 14 | the driver's replay produced a different trace |
| `limit-<name>` | 15 | a section 6 table limit hit |
| crash | signal | assertion or fault in the runtime or simulator |

**The simulator follows Tiger Style** (`docs/TIGER_STYLE.md` at
tigerbeetle/tigerbeetle), because a harness that judges the runtime holds a
stricter bar than the runtime: static allocation at `sim_init` only, sized by
the section 6 constants; bounded loops, no recursion; two assertions per
function on average, positive and negative space, split compound
conditions, in pairs across code paths; explicit integer sizes; units last in
names (`time_ns_virtual`, `stack_bytes`); functions of at most 70 lines; no
dependency beyond libc, `ucontext` and `fork`; a self-test that drives each
invariant to a violation on purpose. Not taken: Zig for tooling, and the
100-column limit; the repository's C23 conventions and formatter apply.

## 8. The harness, the programs, the outputs

**Build.** Make target `sim-runtime-lib` builds `build/libgoo_runtime_sim.a`:
the production runtime objects with `sim_os.o` in place of `goo_os_posix.o`.
Each program under test builds to `build/sim/<name>` with
`-Dmain=goo_sim_program_main`, linked against the sim archive and
`sim_main.o`, which provides `main`: read the seed and limits from the
environment, run the program's main as coroutine 0, run the scheduler until
all coroutines exit or a verdict fires, exit with the verdict code.

**Programs under test, unchanged:** `tests/concurrency/false_deadlock_test.c`,
`deadlock_poll_test.c`, `mt_scheduler_stress.c`, `chan_mt_stress.c`,
`fanin_stress.c`.

**Driver.** `scripts/sim_probe.sh <program> --seeds N [--faults ...]` loops
over seeds, one process per seed, exit status read with no pipe. Output: one
line per seed (`seed verdict steps time_ns_virtual`), then a summary by
verdict. On any non-pass verdict: the trace path, and an automatic replay of
that seed that compares the two traces with `cmp`; a difference is
`replay-mismatch`, and it outranks the original verdict in the report.

**Teeth.** `--self-test` links the wake-site mutant runtime (the same
mutation `scripts/false_deadlock_probe.sh` makes) and expects
`runtime-deadlock-abort` within the seed budget on `false_deadlock_test`; it
also drives one invariant to a violation (a program that unlocks a mutex it
does not hold) and expects `invariant`. The ordinary run links the real
runtime and expects `pass` on every seed. No control depends on git history
or on machine state outside the tree.

## 9. The spike, step by step

1. **The seam, POSIX only, plus `runtime-seam-probe`.** Behaviour-neutral by
   measurement (section 5). Lands regardless of the spike's answer.
2. **The nesting measurement.** One simulated worker running one goroutine
   that crosses a seam call and returns, replayed from a seed with identical
   traces. On failure: fall back to B with the same seam, and record why.
3. **The simulator, the driver, and the false-deadlock program.** Control 1
   red within the budget with replay; control 2 green.
4. **The other four programs** over the same budget. The report of section 3.
5. **Decision.** If step 3 holds: `sim-false-deadlock-probe` joins
   `VERIFY_ALL_DEPS` with the three companion edits (self-test, parity bump,
   census where the base has it), with a seed budget sized to about 20 s of
   wall time, recorded. Then the fault flags become their own task, and the
   time APIs their own cut. If step 3 fails: the report says why, and the
   seam and the grep gate stay.

Seed budget for the spike: 1,000 seeds per program. For the gate: a fixed
list of 256 seeds, so a failure names a seed a person can replay.

## 10. Out of scope

New runtime behaviour of any kind. Simulating the user-facing time APIs, the
far transport, or file I/O. Changing the stress gates: they stay as the
wall-clock complement. Any Bazel target: per the phase 4 decision, the Bazel
branch owns those files, and the make gate comes first.

## 11. Risks

- Nested `ucontext` under one real thread: measured first (step 2), with a
  named fallback.
- A seam call missed in the audit: the grep gate catches `pthread_` and
  `_Thread_local`; a call through a different libc primitive would need a
  new gate line, and the audit lists every OS primitive the two files use
  (none beyond the table today).
- Trace ring roll-off on long runs: the count is kept, and the replay
  compares the retained window plus the count, so a mismatch is still a
  mismatch.
- A program under test that reads real time: none of the five does; the
  seam has no wall-clock call, so such a read would not compile against the
  sim archive.
- Wall time: 1,000 seeds of a stress program may exceed the spike's budget;
  the report records the per-seed time and the gate's 256-seed budget is set
  from it.
