# tests/sim — the deterministic simulator of the runtime scheduler

The runtime's OS calls (threads, mutexes, condition variables, sleep) go
through one seam, `include/goo_os.h`. In production `src/runtime/goo_os_posix.c`
implements it over pthreads. Here `sim_os.c` implements it over ONE real
thread: every simulated OS thread is a `ucontext` coroutine, every seam call
is a scheduling point where a seeded generator picks who runs next, time is
virtual, and every decision is recorded in a trace. The same seed gives the
same interleaving and the same trace, byte for byte.

The design, and the authority for everything below, is
`docs/superpowers/specs/2026-09-03-deterministic-simulation-design.md`.

**Status: the spike answered YES, and two gates run it.**
`sim-false-deadlock-probe` (the corrected runtime passes 256 seeds of
`false_deadlock_test`) and `sim-false-deadlock-selftest` (the probe can go
red: the wake-site mutant aborts, with replay) are in `VERIFY_ALL_DEPS`, so
`make verify-core` runs them. `runtime-seam-probe` is the third gate: the
grep gate that keeps `pthread_` and `_Thread_local` out of the scheduler
files so that no call escapes the seam. The other programs below, and
`sim-entry-test`, are run by hand; the result and the numbers are in the
spec's "Result" section.

**What the simulator cannot reach.** The sim build runs 64 batches per shape
of `fanin_stress`, not the production 4000. A defect that needs many batches
in ONE process is out of the sim build's reach: more seeds explore more
interleavings, but not a longer run. Only the wall-clock `fanin-stress` gate
covers that class.

| File | Role |
|---|---|
| `sim.h` | the constants, the verdict codes, the scheduler's interface |
| `sim_sched.c` | coroutines, the seeded pick, the virtual clock, seeded preemption, the trace ring |
| `sim_os.c` | the seam (`goo_os.h`) over the simulator, plus the invariant checks |
| `sim_main.c` | process entry: reads the environment, runs the program's `main` as coroutine 0 |
| `sim_sched_test.c` | scheduler tests T1-T9 (`make sim-sched-test`) |
| `sim_entry_test.c`, `sim_entry_test.sh` | process-entry facts and the 17 violation cases (`make sim-entry-test`) |
| `sim_invariant_test.c` | a `sync.Mutex` unlocked on a thread that does not own it; the driver's self-test part (b) |
| `evidence/` | the scripts and logs behind the numbers below (see `evidence/README.md`) |

## Run one seed by hand

```sh
make build/sim/fanin_stress          # also builds build/libgoo_runtime_sim.a
mkdir -p /tmp/simtrace
GOO_SIM_SEED=7 GOO_SIM_TRACE_DIR=/tmp/simtrace build/sim/fanin_stress
echo $?                              # the verdict: see the table below
head -1 /tmp/simtrace/7.trace        # the header names the seed, the model and the verdict
```

`build/sim/<name>` builds `tests/concurrency/<name>.c` or `tests/sim/<name>.c`
against the sim archive, with the program's `main` renamed to
`goo_sim_program_main`. A `tests/concurrency` program is compiled with
`-DGOO_SIM_BUILD`, which compiles out `false_deadlock_test`'s refusal to run
with the wake hook unset (the production build keeps it; spec section 3).

A trace is written on every ending, a pass included.

## Run many seeds: `scripts/sim_probe.sh`

```sh
bash scripts/sim_probe.sh fanin_stress --seeds 1000
bash scripts/sim_probe.sh deadlock_poll_test --seeds 1000 --expect runtime-deadlock-abort
bash scripts/sim_probe.sh false_deadlock_test --seeds 64 --fault-spurious-wake
bash scripts/sim_probe.sh --self-test
```

One process per seed, the exit status read with no pipe. One line per seed
(`<seed> <verdict> steps=<n> [replay=...]`), a count by verdict, then one
verdict line that names the artefact, the seeds, the expectation, the
preemption model as the trace headers state it, and the wake hook (`hook
unset`, or `HAND-SET` when `GOO_TEST_WAKE_DELAY_NS` is in the environment).

- **Replay.** Every seed that does not exit 0 runs a second time, and its
  trace must be byte-identical (`cmp`) with the same exit code. A
  difference reads `replay-mismatch`, outranks every other verdict, and the
  probe exits 14.
- **`--expect VERDICT`.** A seed succeeds when it reaches this verdict
  (default `pass`). `deadlock_poll_test` is a real deadlock by construction,
  so it runs with `--expect runtime-deadlock-abort`, and a seed that reads
  `pass` there fails. `replay-mismatch` cannot be expected.
- **Traces.** The trace dir is a fresh `mktemp -d` under `$TMPDIR`. A seed
  that succeeds keeps no trace. A seed that fails keeps its trace (and
  `.out`, `.err`), but only for the first 8 such seeds: a program that fails
  on every seed would keep about 200 MB otherwise. Every seed still runs and
  replays. The verdict line says how many seeds kept files and names the
  dir.
- **A tool failure, never a pass:** a seed that exits 0 with no trace (every
  ending writes one), and an exit 15 whose header has no `limit-<name>`.
- **Ambient variables.** A `GOO_TEST_WAKE_DELAY_NS`, or a `GOO_SIM_*`
  variable the probe does not set itself (for example `GOO_SIM_STEP_LIMIT`),
  is named in the verdict line as `HAND-SET`.
- **Probe exit:** 0 every seed succeeded; 1 a seed failed; 14 a replay
  differed; 2 a tool failure (usage, build, a broken self-test pipeline).
  Note that 2 is also the runtime's `runtime-deadlock-abort` code for ONE
  seed; the probe's own 2 is always a tool failure.
- **`--self-test [--cc "<compiler>"]`** proves the probe can go red: (a) the wake-site mutant
  aborts on at least one of seeds 1..256 and replays byte for byte; (b)
  `sim_invariant_test` reads `invariant` with `unlock-not-owner`; (c) the
  tracked runtime through the mutant's pipeline, and the make-built
  `false_deadlock_test`, pass seeds 1..32; (d) `--expect
  runtime-deadlock-abort` is met by `deadlock_poll_test` and NOT met by
  `false_deadlock_test`; (e) the driver's own rules (no trace, a named
  limit, the cap, the ambient label) against stub programs. It clears every
  `GOO_SIM_*` variable, the wake hook, `CC_PROBE`, `CC` and `CFLAGS` first,
  and builds the mutant with the compiler of `--cc` (default `gcc`; the
  Makefile gate passes `$(CC)`, because CI builds with `make CC=gcc-14`).

## The environment contract (`sim_main.c`)

| Variable | Value | Default |
|---|---|---|
| `GOO_SIM_SEED` | decimal uint64 | 1 |
| `GOO_SIM_TRACE_DIR` | a directory, shorter than 256 bytes; the trace is `<dir>/<seed>.trace` | required |
| `GOO_SIM_STEP_LIMIT` | decimal uint64 > 0 | 1,000,000 |
| `GOO_SIM_TIME_LIMIT_NS` | decimal uint64 > 0, virtual ns | 10,000,000,000 (10 s) |
| `GOO_SIM_PREEMPT_PPM` | 0..1,000,000; 0 turns seeded preemption off | 15,625 (1 decision in 64) |
| `GOO_SIM_FAULT_SPURIOUS_WAKE` | 0 or 1: `goo_cond_wait` may return spuriously | 0 |
| `GOO_SIM_FAULT_THREAD_CREATE_EAGAIN` | 0 or 1: one thread create in 16 fails with `EAGAIN` | 0 |

A malformed value is a usage error, exit 64, before the run starts. The
fault flags are not recorded in the trace header (a known gap).

**Seeded preemption** (spec section 6) is ON by default: at each scheduling
decision the seed deschedules the picked coroutine with probability ppm/10^6,
for a log-uniform duration of 2^10..2^24-1 ns (about 1 us to 16.8 ms). With
ppm > 0 each decision also costs 1,000 ns of virtual time, and every sleeper
due by then wakes before the pick. At ppm 0 neither rule applies, no random
number is drawn for it, and every trace is byte-identical to the trace the
same seed gave before the model existed.

## Verdicts and exit codes

| Verdict | Exit | Meaning |
|---|---|---|
| `pass` | 0 | the program's `main` returned 0 on coroutine 0, and its own result check held |
| `runtime-deadlock-abort` | 2 | the runtime's `goo_deadlock_abort` (the trace header calls this `exit`; classify by the exit code) |
| `sim-deadlock` | 10 | nothing runnable, nothing sleeping |
| `invariant` | 11 | a seam check failed; the header's `detail=` names which |
| `step-limit` | 12 | `GOO_SIM_STEP_LIMIT` reached |
| `time-limit` | 13 | `GOO_SIM_TIME_LIMIT_NS` reached |
| `replay-mismatch` | 14 | the driver's replay differed (the driver's verdict, never a process's) |
| `limit-<name>` | 15 | a static table is full; the header says `verdict=limit-<name>` |
| `crash-<n>` | other | a signal or another exit code (an assertion is 134, SIGABRT) |

The invariants (`sim_os.c`), by their `detail=` name: `unlock-not-owner`
(this also covers a double unlock), `recursive-lock`, `wait-without-mutex`
(a wait without the mutex, or with another thread's mutex: one name for
both), `lock-order` (a thread that holds the scheduler mutex takes no other
mutex), `exit-holding-mutex`, `free-held-mutex`, `free-cond-with-waiters`,
`join-detached`, `join-self`, `unknown-mutex`. `make sim-entry-test` drives
each to its own verdict. `unknown-mutex` has four cases: a mutex the seam
never made, a freed handle, a copy of a live mutex at another address, and
the bytes of a freed mutex written back at the address a new mutex reuses
(see "The mutex handle" below).

**`unlock-not-owner` from a Go program is a TRUE finding.** `sync.Mutex` is
a `PTHREAD_MUTEX_DEFAULT` mutex owned by the worker OS thread that locked it.
Go allows an unlock from another goroutine, which can run on another worker;
POSIX makes that undefined behaviour. The simulator reports it as
`invariant`, and that verdict is right: it is a runtime defect, recorded for
the user outside this plan.

## The limits (`sim.h`, compile-time)

| Constant | Value | Verdict when exceeded |
|---|---|---|
| `SIM_THREADS_MAX` | 17 (16 workers + main) | `limit-threads` |
| `SIM_STACK_BYTES` | 262,144 per coroutine | no verdict of its own |
| `SIM_WAITERS_MAX` | 17 per wait object | none: a `static_assert` keeps it at least `SIM_THREADS_MAX`, so it cannot fill |
| `SIM_MUTEXES_MAX` | 16,384 LIVE mutexes (64 until Task 7 fix round 1) | `limit-mutexes` |
| `SIM_QUARANTINE_MAX` | 16,384 mutexes FREED over a run (their storage is kept) | `limit-freed-mutexes` |
| `SIM_TRACE_ENTRIES` | 65,536 records; older records roll off, the count is kept | none |

A mutex slot is freed only by `goo_mutex_free`. No program under test
frees a channel, and codegen emits no `goo_chan_free`, so every channel a
program makes holds a slot until the process ends. `fanin_stress` makes one
channel per batch: 8,000 in the full program (8,001 slots), 128 in the sim
build (about 129 slots). The table is sized for headroom, not for that
program: ordinary Go programs make many channels, and 16,384 is a power of
two. History: the first cut's 64 slots stopped the full program in batch 64
on every seed.

A freed mutex keeps its storage (see "The mutex handle"), so the frees are
counted: the 16,385th free ends the run with `limit-freed-mutexes`, and the
memory is bounded at about 16,384 times `sizeof(goo_mutex_t)`.

**The mutex handle.** Each `goo_mutex_t` carries a tag -- a magic number,
its slot index and the slot's generation -- in the bytes of its
`pthread_mutex_t`, which only production uses; `struct goo_mutex` does not
change. A lookup is O(1): it checks the magic, the index range, that the
slot names this address, and the generation, each on its own line, and any
failure reads `unknown-mutex`. A free clears the magic and changes the
generation, and keeps the object: freed mutex storage is never returned to
`malloc`, so a freed handle can never share an address with a live mutex,
and every tag read is of live storage. A new mutex takes the head of a free
list or the next untouched slot. No seam call walks the table, so 16,384 slots cost a lock what 64 did.
The `limit-mutexes` case makes 16,385 mutexes in about 3 ms; the
`limit-freed-mutexes` case makes and frees 16,385.

## Read a trace

```
# goo-sim trace v1 seed=1 preempt_ppm=15625 preempt_e_min=10 preempt_e_max=23 step_cost_ns=1000 steps=86 now_ns=2050660 records=93 retained=93 verdict=exit detail=the process called exit
# step now_ns thread what choice
1 1000 - start 0
2 2000 0 create 0
6 6000 3 wait_mutex 2
6 6000 2 wake_one 3
...
<step> <now_ns> <thread> preempt <coroutine> d_ns=<d>
```

- The header: the seed; the preemption model (absent at ppm 0, which means
  off); `steps` (scheduling decisions), `now_ns` (virtual time at the end),
  `records` (all records written) and `retained` (the ones still in the
  ring); the verdict and its detail.
- A record: the step, the virtual time, the thread that made the call (`-`
  is the real thread), the record kind, and the choice: the thread picked
  or woken. The kinds are the seam calls (`start`, `create`, `lock`,
  `try_lock`, `unlock`, `signal`, `broadcast`, `sleep`, `join`, `detach`,
  `exit`, `spurious`, `create_fail`), the blocks (`wait_mutex`,
  `wait_cond`), the wakes (`wake_one`), and `preempt`, which names the
  descheduled coroutine and its duration. A wake is a record of its own,
  so two records can share a step.

## Replay a seed

Run the same binary with the same environment and compare:

```sh
GOO_SIM_SEED=18 GOO_SIM_TRACE_DIR=/tmp/a build/sim/false_deadlock_test; echo $?
GOO_SIM_SEED=18 GOO_SIM_TRACE_DIR=/tmp/b build/sim/false_deadlock_test; echo $?
cmp /tmp/a/18.trace /tmp/b/18.trace && echo identical
```

A replay under a different `GOO_SIM_PREEMPT_PPM` is a different experiment;
the header shows the model, so compare headers first. `sim_probe.sh` replays
every non-pass seed itself.

## The spike's numbers

Every row below: the default model (ppm 15,625, exponents 10..23, step cost
1,000 ns) unless the row says ppm 0; the wake hook unset; faults off; default
limits; measured 2026-11-08 on this machine (Linux 7.2.15, Fedora 45). Wall
time per seed is through the driver, which adds about 2.4 ms of forks per
seed over the program alone. Max steps is over the seeds of that row.

### The pass bar (spec section 3)

| Program | Runtime | Seeds | Verdicts | ms/seed | Max steps | Limits hit | Commit | Instrument |
|---|---|---|---|---|---|---|---|---|
| `false_deadlock_test` | wake-site mutant | 1..256 | runtime-deadlock-abort 33 (first seed 18, 114 steps, replay identical), pass 223 | 4.97 (4.95 to 6.82 in three later runs) | 490 | none | `a7135bfe`, again at `8233d02f` | `bash scripts/sim_probe.sh --self-test`, part (a) |
| `false_deadlock_test` | wake-site mutant, ppm 0 | 1..256 | runtime-deadlock-abort 0, pass 256 | | | none | `8233d02f` | the self-test's sed and link, run directly with `GOO_SIM_PREEMPT_PPM=0`; the same script at the default model gives 33 aborts, first seed 18 (the positive control) |
| `false_deadlock_test` | corrected | 1..1000 | pass 1000 | 3.86 | 910 | none | `a7135bfe` | `bash scripts/sim_probe.sh false_deadlock_test --seeds 1000` |

All three conditions of the pass bar hold: the mutant reaches
`runtime-deadlock-abort` from a seed with the hook unset, the corrected
runtime passes every seed under the same model, and the failing seed replays
byte for byte. Preemption is what opens the window: at ppm 0 the mutant does
not abort on any seed.

### The other four programs (spec section 9, step 4)

| Program | Seeds | Model | Verdicts | ms/seed | Max steps | Max virtual time | Limits hit |
|---|---|---|---|---|---|---|---|
| `deadlock_poll_test` (`--expect runtime-deadlock-abort`) | 1..1000 | default | runtime-deadlock-abort 1000 | 10.50 | 388 | 31.9 ms | none |
| `mt_scheduler_stress` | 1..1000 | default | pass 1000 | 36.99 | 32,136 | 0.90 s | none |
| `chan_mt_stress` | 1..1000 | default | pass 1000 | 24.96 | 28,189 | 0.78 s | none |
| `fanin_stress` (64 batches per shape in the sim build) | 1..1000 | default | pass 1000 | 13.46 | 19,752 | 0.74 s | none |
| `deadlock_poll_test` (`--expect runtime-deadlock-abort`) | 1..256 | ppm 0 | runtime-deadlock-abort 256 | 11.36 | 101 | | none |
| `mt_scheduler_stress` | 1..256 | ppm 0 | pass 256 | 38.77 | 28,449 | | none |
| `chan_mt_stress` | 1..256 | ppm 0 | pass 256 | 25.72 | 23,480 | | none |
| `fanin_stress` (64 batches per shape in the sim build) | 1..256 | ppm 0 | pass 256 | 13.56 | 17,021 | 0.13 s | none |

Commit `8233d02f`, except the two `fanin_stress` rows: commit `bc4352bd`
(16,384 mutex slots, and 64 batches per shape in the sim build). Instrument: `bash scripts/sim_probe.sh <program>
--seeds <n> [--expect runtime-deadlock-abort]`, with `GOO_SIM_PREEMPT_PPM=0`
for the ppm-0 rows. At `1c7cd52d`, `mt_scheduler_stress` and
`chan_mt_stress` pass 256 of 256 (max 31,910 and 28,189 steps), and the
pass bar gives the same numbers: the mutant aborts on 33 of 256 seeds, first
seed 18, and the corrected runtime passes 256 of 256 (max 659 steps). Max virtual time: the largest `now_ns` in the trace headers of
seeds 1..1000, one direct run per seed. The deadlock_poll ms/seed is high
for its step count because every seed exits non-zero, so every seed runs
twice (the replay) with the replay's extra forks; one direct run of a seed
takes about 1 ms.

**`fanin_stress` runs 64 batches per shape under the simulator, 4000 in
production.** Each batch makes a new unbuffered channel and frees none.

- At `8233d02f` (64 mutex slots) every seed stopped at `limit-mutexes` in
  batch 64 (`gdb -nx` `dprintf` counts on seeds 1, 2, 500 and 1000: 65
  `goo_mutex_new`, 64 `goo_make_chan`). The table now holds 16,384.
- At `1c7cd52d` (4000 batches per shape) every seed reached a run limit
  while it still made progress: `time-limit` at the default model (seed 1:
  3,167 of 8,000 batches), `step-limit` at ppm 0 (7,673 batches). Each batch
  costs virtual time, because `goo_scheduler_wait` polls with a 0.5 ms sleep
  (`src/runtime/concurrency.c:185`). With the limits raised through the
  environment (a diagnostic, not a default), seeds 1..8 passed: about 30 s
  and 1.1 M steps at the default model, 8.0 s and 1.05 M steps at ppm 0.
- So the sim build (`GOO_SIM_BUILD`) runs 64 batches per shape. Sized from
  the 4000-batch program over seeds 1..64 at the default model: the worst
  seed took 138.7 steps and 3.879 ms per batch. The bar is 10% of each
  default limit (100,000 steps, 1 s); 1 s / 3.879 ms = 257 batches, and half
  of that is 2 x 64. Measured at `bc4352bd`: the worst of seeds 1..1000 took
  19,752 steps (2.0%) and 0.743 s (7.4%). The simulator explores
  interleavings by seed, so more seeds add more than more batches in one
  seed. The production build still runs 4000 (`make fanin-stress` prints
  `x 4000 batches`), and the sum check per batch is the same in both.

### The nesting measurement (Task 4, approach A)

| Measurement | Result | Commit | Instrument |
|---|---|---|---|
| T3 nesting, seed 11 | `sim_sched_test: T3 nesting: PASS (steps=15, trace bytes=347)` | `ff97af61`; the same line at `7f211b09` | `make sim-sched-test` |
| T3 workload, seeds 1..1000, `-O0` and `-O2` | 1000 pass at each level, 0 crashes, 0 replay mismatches, traces byte-identical across levels; 856 seeds switch threads inside the nested coroutine | `ff97af61`, again at `8233d02f` | `tests/sim/evidence/nesting-sweep/build_and_sweep.sh <dir> 1000` |

These were measured BEFORE seeded preemption existed, at the then-only
model, which is ppm 0 now. The re-run at `8233d02f` passes ppm 0 explicitly
and gives the same counts.
