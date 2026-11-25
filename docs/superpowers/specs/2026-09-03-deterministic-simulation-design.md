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
   the false deadlock, a real defect, found from a seed. The mutant runs
   with `GOO_TEST_WAKE_DELAY_NS` UNSET and seeded preemption (section 6) at
   its default. The hand-set wake window is not part of the pass bar: the
   seed must open the window, as a loaded OS does.
2. **Negative control.** The corrected runtime reaches `pass` on every seed
   of the same budget, for the same program, under the same preemption
   model.
3. **Replay.** The failing seed of control 1, run again, produces a trace
   byte-identical to the first. The harness asserts this on every non-pass
   verdict, always.

Numbers recorded by the spike: seeds to first failure, wall time per seed,
the verdict table for every program, and which limits were hit.

**The hook refusal (ruling, Task 6a fix round 1).** `false_deadlock_test.c`
refuses to run with `GOO_TEST_WAKE_DELAY_NS` unset in the PRODUCTION build,
because mutant 2 of `scripts/false_deadlock_probe.sh --self-test` needs the
refusal. The sim build compiles the refusal out: the Makefile's
`build/sim/%` rule for `tests/concurrency` sets `GOO_SIM_BUILD`.
`scripts/sim_probe.sh` (Task 6, commit `a7135bfe`) runs the program with the
hook UNSET by default. A `GOO_TEST_WAKE_DELAY_NS` in the caller's
environment passes through, and the verdict line then names it as
`HAND-SET`, so a run with a hand-set window cannot pass for an unset one.
The driver's self-test builds its mutant with `-DGOO_SIM_BUILD`, the flags
of the `build/sim/%` rule, and it clears every `GOO_SIM_*` variable and the
hook from its environment first. (Corrected in Task 7: an earlier text of
this paragraph said that the driver set the hook to 1 ns and that its
self-test built without the define. Both were true only of a draft.)

**Measured (Task 6a fix round 1, the committed program, hook unset, default
model, seeds 1..256).** The mutant aborts on 33 seeds, the first at seed 18
(114 steps), and the replay is byte-identical. With preemption off, the
mutant aborts on 0 seeds. The corrected runtime passes 256 of 256, at most
659 steps.

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
| `SIM_MUTEXES_MAX` | 16,384 | LIVE mutexes: the scheduler mutex plus one per channel, and no program under test frees a channel. Headroom, not the need of one program: the sim build of `fanin_stress` needs about 129, and ordinary Go programs make many channels. A power of two. Was 64 until Task 7 fix round 1, when the full `fanin_stress` (8,001 mutexes) stopped at `limit-mutexes` in batch 64 |
| `SIM_QUARANTINE_MAX` | 16,384 | mutexes FREED over a run: a freed mutex keeps its storage (below), so the count bounds that memory. The next free is `limit-freed-mutexes` (Task 8) |
| `SIM_TRACE_ENTRIES` | 65,536 | 40 bytes each (a `static_assert` in `sim_sched.c` holds the size), 2,621,440 bytes (2.5 MiB); older entries roll off, the count is kept |
| `GOO_SIM_STEP_LIMIT` (env) | default 1,000,000 | the run bound |
| `GOO_SIM_TIME_LIMIT_NS` (env) | default 10 s virtual | the clock bound |

**The mutex handle (Task 7 fix round 1, 2026-11-08).** The first cut found
a mutex by a search of the whole table at every seam call; at 16,384 slots
that costs 256 times the 64-slot search. Now each `goo_mutex_t` carries a
tag -- a magic number, its slot index and the slot's generation -- in the
bytes of its `pthread_mutex_t`, which only `goo_os_posix.c` uses (the sim
archive never links it), so `struct goo_mutex` does not change. A lookup is
O(1) and checks, each on its own line, the magic, the index range, that the
slot names this address, and the generation. A free clears the magic and
changes the slot's generation, and keeps the object (fix round 2): freed
mutex storage is never returned to `malloc`, because a new mutex at the
freed address would make the stale pointer equal to a live handle, and a
use after free would then lock the wrong mutex with no verdict; a new mutex takes the head of a free list or
the next untouched slot, so no seam call walks the table. A handle the seam
did not make, a freed handle, a copy at another address, and stale bytes
written back at a reused address each read `unknown-mutex`
(`sim_entry_test` cases `unknown-mutex`, `freed-mutex`, `freed-then-new`,
`copied-mutex`, `stale-generation`). No trace record names a slot or a handle.

### Seeded preemption (Task 6a, 2026-11-08)

**Why.** Task 6 found that the wake-site mutant read `pass` on 256 of 256
seeds at a 1 ns wake delay. The virtual clock advanced only when nothing was
runnable, so scheduler latency cost zero virtual time. A real OS deschedules
a running thread for milliseconds under load, and that is the window in
which the task 8 false deadlock occurs in production. The simulator now
models it, under the seed.

**Where.** At every scheduling decision the simulator already makes (each
run of the seeded pick in `sim_yield_point`), and only there.

**What.** After the pick selects the coroutine `c` that runs next, the seed
decides whether the OS deschedules `c`:

- With probability `P` parts per million, `c` does not run now. It is
  parked as a sleeper with the deadline `now + d`, and the pick runs again
  over the coroutines that remain runnable, or the clock jumps if none
  remain. The second pick is a decision too, so it can also preempt. When
  its deadline comes, `c` is runnable again and continues from the point
  where it is parked. A preemption is NOT a seam call: the program and the
  runtime see it only as elapsed virtual time.
- `d` is log-uniform: an integer exponent `e` uniform in
  `[SIM_PREEMPT_EXPONENT_MIN, SIM_PREEMPT_EXPONENT_MAX]`, then an integer
  uniform in `[2^e, 2^(e+1))`, in nanoseconds.

**Parameters.** Fixed by the Task 6a brief before any measurement. They do
not change after a result is seen.

| Name | Value | Meaning |
|---|---|---|
| `GOO_SIM_PREEMPT_PPM` (env) | default 15,625 (`SIM_PREEMPT_PPM_DEFAULT`) | 1 decision in 64; 0 is off; above 1,000,000 is a usage error (exit 64) |
| `SIM_PREEMPT_EXPONENT_MIN` | 10 | about 1 us |
| `SIM_PREEMPT_EXPONENT_MAX` | 23 | 8.4 ms to 16.8 ms at the top |
| `SIM_PREEMPT_NS_MAX` | 16,777,215 | `2^24 - 1`; written as a literal, so a raised exponent trips the bound assertion |

A Linux CFS slice is a few milliseconds, and a thread descheduled for up to
about 16 ms under load is ordinary.

**Default: ON.** Preemption is not a fault. A fault is an error return that
the OS may give. Preemption is the normal behaviour of every preemptive OS,
and a run without it models an OS that never deschedules a thread, which
does not exist. The faults of section 7 stay OFF by default.

**Step cost and the wake rule (fix round 1, I1).** With the model on
(ppm > 0), each scheduling decision costs `SIM_STEP_COST_NS` = 1,000 ns of
virtual time, and at each decision every sleeper whose deadline is at or
before the new time becomes runnable before the pick. This covers a
preempted coroutine and an ordinary sleeper. Reason: the clock otherwise
advances only when nothing is runnable. A coroutine that spins through seam
calls without a sleep (a `goo_mutex_try_lock` loop, or a `select` with
`default` through `goo_chan_try_send`) stays runnable, so the clock moves
only when the seed preempts every runnable coroutine at once, and with a
rare preemption it does not move at all: the run ends in a false
`step-limit`. `sim_sched_test` T9b shows it at ppm 1: with the step cost the
sleeper wakes after 5,004 steps, and without it (a mutant) the run is
`step-limit`. When a preemption leaves nothing runnable, the clock jumps to
the deadline, and the cost of that next decision is inside the jump. At
ppm 0 neither rule applies.

**Determinism.** The preemption model uses the same seeded generator as the
pick: one test draw per decision, and two more draws on a hit (the exponent
and the mantissa of `d`). The step cost draws nothing. With
`GOO_SIM_PREEMPT_PPM=0` the model draws NO random number and charges no
step cost, so every trace is byte-identical to the trace of the same seed
before the model existed. `sim_sched_test` T5 pins this on the T1-T3
traces.

**When the time limit binds (M3).** Under the default model the mean `d` is
the mean over the 14 exponents of `(2^e + 2^(e+1) - 1) / 2`, which is
1,797,448.6 ns. A decision preempts with p = 15,625 / 10^6 = 1/64, so a
decision adds at most 1,000 + 1,797,448.6 / 64 = 1,000 + 28,085.1 =
29,085.1 ns on average. With the default time limit of 10^10 ns:

- when every `d` is on the clock's path (one runnable coroutine, so each
  preemption makes the clock jump), the time limit binds after about
  10^10 / 29,085.1 = 343,818 decisions (356,060 without the step cost);
- when no `d` is on the clock's path (another coroutine is always
  runnable), only the step cost moves the clock, and the time limit binds
  after 10^10 / 1,000 = 10,000,000 decisions.

The default step limit is 1,000,000 decisions. Thus in the first case the
time limit binds first, and in the second case the step limit binds first.

**Trace.** A preemption appends one record with `what` = `preempt`, the
preempted coroutine as the choice, and `d` as a sixth field:
`<step> <now_ns> <thread> preempt <coroutine> d_ns=<d>`. The record stays 40
bytes: `d` takes the 4 bytes that were alignment padding. Every other record
keeps its five fields.

**Bounds.** `d` is at most `SIM_PREEMPT_NS_MAX`, and the simulator asserts
it. A preemption never targets the real thread (`SIM_NONE`), and the
simulator asserts that too. Each preemption takes one step, so the step
limit bounds a chain of preemptions; the time limit also applies. A
preempted coroutine is a sleeper with a deadline: it is not runnable, and it
cannot cause `sim-deadlock`.

**The trace header names the model.** A replay under a different model is
a different experiment. With the model on, the header has
` preempt_ppm=<ppm> preempt_e_min=10 preempt_e_max=23 step_cost_ns=1000`
after `seed=`. At ppm 0 the header has no such field, so a ppm-0 trace keeps
its pre-model bytes: no field means the model is off. The fault flags are
still not in the header.

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
| `pass` | 0 | the program's main returned 0 on coroutine 0, and its own result check held |
| `runtime-deadlock-abort` | 2 | the runtime's `goo_deadlock_abort` |
| `sim-deadlock` | 10 | nothing runnable, nothing sleeping |
| `invariant` | 11 | a section 7 check failed |
| `step-limit` | 12 | `GOO_SIM_STEP_LIMIT` reached |
| `time-limit` | 13 | `GOO_SIM_TIME_LIMIT_NS` reached |
| `replay-mismatch` | 14 | the driver's replay produced a different trace |
| `limit-<name>` | 15 | a section 6 table limit hit |
| crash | signal | assertion or fault in the runtime or simulator |

**What `pass` means (ruling, 2026-11-08, Task 5).** An earlier draft said
"every coroutine exited". That cannot hold without a runtime change: the
workers leave their loop only through `goo_scheduler_shutdown`, and no
program under test calls it. So a return from the program's main is
`exit(status)` on coroutine 0, as in production, where the workers still
run when main returns. The limit: a program that does not wait for its
goroutines can leave one stuck and still pass. Go and the production
runtime behave the same way.

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
`fanin_stress.c`. One exception (ruling, Task 7 fix round 2): under
`GOO_SIM_BUILD`, `fanin_stress.c` runs 64 batches per shape instead of 4000,
because 8,000 batches need about 30 s of virtual time and 1.1 M steps per
seed at the default model, over both default run limits. The value is sized
so that the worst seed uses under 10% of each limit; the production build is
unchanged.

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
- A program under test that reads real time. This spec said first that
  none of the five does, and that such a read would not compile against the
  sim archive. Both parts were wrong. The risk occurred once:
  `tests/concurrency/false_deadlock_test.c:57` called `nanosleep` directly,
  which is libc, not the seam, so it linked and slept 1 ms in REAL time
  under the simulator, at no cost in virtual time. Task 6a corrected it: the
  sleep goes through `goo_platform_sleep_ns`, which is the same `nanosleep`
  in production. No gate catches the next such call; the runtime-seam grep
  gate (`scripts/runtime_seam_probe.sh`) scans `concurrency.c`,
  `channels.c` and `sync.c` in `src/runtime`, not the programs under test.
- Wall time: 1,000 seeds of a stress program may exceed the spike's budget;
  the report records the per-seed time and the gate's 256-seed budget is set
  from it.

## 12. Result (Task 8, 2026-11-08)

**Decision: YES.** All three conditions of section 3 hold, and no stress
program has an open finding. `sim-false-deadlock-probe` and
`sim-false-deadlock-selftest` join `VERIFY_ALL_DEPS`. The consequences are in
section 9, step 5.

Every number below was measured again on 2026-11-08 at commit `2d8f3ab0`
(the code that Task 7 left), on one machine (Linux 7.2.15, Fedora 45), with
the wake hook UNSET, the default preemption model (ppm 15,625, exponents
10..23, step cost 1,000 ns), faults off, and the default limits. The
instrument is `scripts/sim_probe.sh`, one process per seed. Commit
`a3fee154` (Task 8) changes no trace record: 100 traces of five programs at
ppm 0 and at the default model are byte-identical to the `2d8f3ab0` build,
and the 1,000-seed total of 291,275 steps for `false_deadlock_test` is the
same before and after.

| Condition | Result | Instrument |
|---|---|---|
| 1. Positive control: the wake-site mutant reaches `runtime-deadlock-abort` | 33 of seeds 1..256 abort; the first is seed 18, at 114 steps; the replay is byte-identical | `bash scripts/sim_probe.sh --self-test`, part (a) |
| 2. Negative control: the corrected runtime passes every seed | `pass` on 1000 of 1000 (seeds 1..1000), at most 910 steps | `bash scripts/sim_probe.sh false_deadlock_test --seeds 1000` |
| 3. Replay | seed 18 replays byte for byte; the driver replays every non-pass seed, always | the same self-test |

| Program | Seeds | Verdicts | Max steps |
|---|---|---|---|
| `mt_scheduler_stress` | 1..1000 | pass 1000 | 32,136 |
| `chan_mt_stress` | 1..1000 | pass 1000 | 28,189 |
| `fanin_stress` (64 batches per shape in the sim build) | 1..1000 | pass 1000 | 19,752 |
| `deadlock_poll_test` (`--expect runtime-deadlock-abort`) | 1..1000 | runtime-deadlock-abort 1000, as built | 388 |

Command for the last four rows: `bash scripts/sim_probe.sh <program> --seeds
1000`, with `--expect runtime-deadlock-abort` for `deadlock_poll_test`. No
run hit a limit. The full table, with the wall time and the ppm-0 rows, is in
`tests/sim/README.md`.

**Two facts about the method.**

- The first preemption model could not open the timing window. With the hook
  at 1 ns and no seeded preemption, the mutant passed 256 of 256 seeds,
  because virtual time advances only when nothing can run, so scheduler
  latency cost no time. Task 6a added seeded preemption (section 6), with the
  parameters fixed before any measurement. With preemption off, the mutant
  still passes 256 of 256 seeds: the seed, through the model, opens the
  window.
- The simulator found one real runtime defect, recorded and not fixed by this
  plan. `sync.Mutex` is a `PTHREAD_MUTEX_DEFAULT` mutex that a worker OS
  thread owns. Go allows an unlock from another goroutine, and that goroutine
  can run on another worker. POSIX makes that unlock undefined behaviour. The
  simulator reports it as `invariant`, detail `unlock-not-owner`
  (`tests/sim/sim_invariant_test.c`, self-test part (b)).

**The gate.** `sim-false-deadlock-probe` runs `false_deadlock_test` over 256
seeds, and `sim-false-deadlock-selftest` runs the self-test. Measured on
2026-11-08 at commit 6f8400c7, three runs each, one `time make <target>` per
run: `sim-false-deadlock-probe` 1.38 to 1.40 s, `sim-false-deadlock-selftest`
2.54 to 2.56 s, `sim-sched-test` 0.24 to 0.25 s and `sim-entry-test` 0.29 s,
so about 4.5 s for the four gates. The Task 8 implementer and the final
reviewer measured 1.4 to 1.5 s and 2.5 to 2.7 s for the first two. The
section 9 target was about 20 s together. The two test targets
`sim-sched-test` and `sim-entry-test` are in `VERIFY_ALL_DEPS` too. Their
test cases are the negative controls: the entry test has 17 violation cases,
each of which must fire its own check, and the scheduler test has nine
tests with fixed expected values. A defect that needs many batches in one
process (the production `fanin_stress` runs 4000) is out of the sim build's
reach; only the wall-clock `fanin-stress` gate covers it.
