# SPIN models

Formal models of Goo runtime protocols. Each model is checked with the
vendored SPIN (`scripts/spin_bootstrap.sh` prints the binary path).

Contents:

- `deadlock_detector.pml` — the runtime's deadlock detector as it is today.
- `run_deadlock_model.sh` — build and run that model.
- `evidence/` — a committed counterexample trail and its readable trace.

---

## `deadlock_detector.pml` — the deadlock detector as it is

### What it models

The C under test, all on branch `build/bazel-phase4-probes` at `9311080`:

| C code | Model |
|---|---|
| `goo_scheduler_wait()` poll loop, `src/runtime/concurrency.c:143-201` | `proctype Main` |
| `goo_sched_block_begin()`, `concurrency.c:296-324` | `inline block_begin` |
| `goo_sched_block_end()`, `concurrency.c:327-335` | `inline block_end` |
| `scheduler_get_next_goroutine()`, `concurrency.c:435-446` | the `readyq--` step in `proctype Goroutine` |
| `goo_goroutine_exit()`, `concurrency.c:343-347` | the `gstate = G_DONE` then `nlive--` steps |
| `goo_chan_send()` buffered path, `src/runtime/channels.c:277-329` | `inline do_send` |
| `goo_chan_recv()` buffered path, `src/runtime/channels.c:422-479` | `inline do_recv` |
| `goo_deadlock_abort()`, `src/runtime/deadlock.c:44-48` | `prog_exit = 1` and `goto stopped` |

Modelled state: one channel of capacity `CAP`, the channel mutex
(`ch->mutex`), the two condition variables (`not_full`, `not_empty`), the
scheduler mutex, `deadlock_detector.blocked_goroutines`,
`stats.num_goroutines`, the ready queue as a count, `main_in_wait`, and the
poll loop's `asleep_streak`.

Lock order is faithful: `goo_sched_block_begin()` takes the scheduler mutex
while the caller already holds the channel mutex, and `main` takes only the
scheduler mutex. So `main` can read `blocked_goroutines` while the
incrementing thread still holds the channel mutex and has not yet parked.

`main_in_wait` is modelled because the design names it. Note that the C code
**writes it and never reads it** — `grep -rn main_in_wait src/ include/` finds
one write in `concurrency.c`, one reset in `deadlock.c`, and the field
declaration. It steers nothing.

### The five states of a goroutine

```
G_READY     in ready_queue, no worker has popped it yet
G_RUNNING
G_WAITING   parked in pthread_cond_wait, not signalled
G_SIGNALLED signalled, has not yet reacquired the channel mutex and
            returned through goo_sched_block_end
G_DONE
```

`G_SIGNALLED` has no counterpart in the C code. It is internal to the pthread
condition variable, and the detector cannot see it. That is the whole point of
the model: `blocked_goroutines` still counts a `G_SIGNALLED` goroutine.

### What it abstracts away, and why

1. **Time.** There is no clock. `goo_platform_sleep_ns(500000)` is dropped; a
   poll is a step `main` takes whenever the scheduler runs it. The C code's
   threshold of 100 polls of 0.5 ms is therefore modelled as a count `K` of
   polls, not as 50 ms.
2. **The wake delay has no bound.** A `G_SIGNALLED` goroutine proceeds only
   when Promela's interleaving runs it, and Promela puts no bound on that. This
   is the hypothesis under test, stated as an abstraction so it can be seen.
3. **Data values.** The channel carries no values, only an occupancy count.
4. **Worker threads.** Every goroutine can be popped from the ready queue at
   any time, as if `GOMAXPROCS >= N`. A smaller worker pool would leave the
   ready queue non-empty, which RESETS `asleep_streak` and makes an abort
   HARDER. The abstraction is therefore the one that favours the detector,
   and it also matches the default (`goo_default_thread_count()` returns the
   online CPU count, clamped to 16).
5. **`close`, unbuffered channels, `select`, `goo_yield`, panics, scheduler
   shutdown.** All out. The `stopped = !g_scheduler->running` term of the poll
   loop is therefore always 0 in the model.
6. **`self->waiting_on_channel` and `waiting_for_send`.** Out. `channels.c`
   sets them and the detector never reads them.
7. **`exit(2)` at the abort.** Modelled as `prog_exit = 1`, which lets every
   parked goroutine leave its wait and terminate. Without it, a CORRECT abort
   would strand the parked goroutines and pan would report an invalid end
   state for a case that is not a defect.

`gstate` is set to `G_WAITING` BEFORE `block_begin`, because that is where the
C sets `self->state = GOO_GOROUTINE_BLOCKED` (`channels.c:291`, `:431`), and it
returns to `G_RUNNING` AFTER `block_end`, because that is where the C sets
`self->state = GOO_GOROUTINE_RUNNING` (`channels.c:304`, `:444`).

### The claim

```
real_deadlock  ==  every goroutine that is not DONE is WAITING
                   and at least one goroutine is not DONE
                   and readyq == 0
                   and nlive > 0
```

`assert(rd)` sits at the abort point, immediately where the C calls
`goo_deadlock_abort()`. A counterexample is therefore an abort of a live
program.

"at least one goroutine is not DONE" is read off `gstate`, not off `nlive`.
`goo_goroutine_exit()` sets `goroutine->state = DONE` one step before it
decrements `stats.num_goroutines`, and a poll that lands in that window would
otherwise satisfy "every live goroutine is WAITING" vacuously with every
goroutine already DONE. The extra test only ever makes `real_deadlock` HARDER
to satisfy, so it strengthens the claim. It never excuses an abort.

### Parameters

Set with `spin -D`, which passes them to the C preprocessor. The runner does
this for you.

| Name | Meaning | Default |
|---|---|---|
| `N` | goroutines, 2..4 | 2 |
| `K` | `asleep_streak` threshold; the C code uses 100 | 3 |
| `DETECTOR` | 1 detector on, 0 detector never aborts | 1 |
| `CAP` | channel buffer capacity | 1 |
| `OPS` | channel operations per goroutine | 2 |

Goroutine `i` sends when `i` is even and receives when `i` is odd. So `N = 2`
and `N = 4` balance the sends against the receives, and `N = 3` does not: it
has two senders and one receiver, so a genuine deadlock is reachable there.

### Commands

```
./tests/models/run_deadlock_model.sh N K [DETECTOR]
```

Exit status: 0 when pan reports 0 errors, 1 when pan reports an error, 2 when
the arguments are wrong or spin, cc or pan fails to give a verdict.

`KEEP_DIR=<dir>` copies the model, `pan.log`, any `.trail` and a `spin -t -p`
trace into `<dir>`. `PAN_ARGS` passes extra flags to `./pan`. The work
directory is always a fresh `mktemp -d` and is removed on exit, so `pan.*`
files never land in the tree.

By hand, which is what the runner does:

```
SPIN=$(bash scripts/spin_bootstrap.sh | tail -1)
cp tests/models/deadlock_detector.pml "$WORK/" && cd "$WORK"
"$SPIN" -DN=2 -DK=1 -DDETECTOR=1 -a deadlock_detector.pml
cc -O2 -DSAFETY -o pan pan.c
./pan
"$SPIN" -t -p -DN=2 -DK=1 -DDETECTOR=1 deadlock_detector.pml
```

The `-D` flags must be the same on `spin -a` and on `spin -t`, or the replay
reads a different model.

To replay the committed trail:

```
cp tests/models/deadlock_detector.pml "$WORK/"
cp tests/models/evidence/deadlock_detector_N2_K1.trail \
   "$WORK/deadlock_detector.pml.trail"
cd "$WORK" && "$SPIN" -t -p -DN=2 -DK=1 -DDETECTOR=1 deadlock_detector.pml
```

### Results

Instrument: Spin Version 6.5.2 (6 December 2019), vendored, built by
`scripts/spin_bootstrap.sh`; `cc (GCC) 16.2.1 20260819 (Red Hat 16.2.1-2)`
with `-O2 -DSAFETY`; Linux 7.1.12-200.fc44.x86_64, 32 cores. Taken
2026-09-02 on branch `build/bazel-phase4-probes` at `9311080`.

`errors` and `states` come from pan's own summary. For a FAIL, pan stops at
the first error, so the state count is what it explored before the error and
not the whole state space — the runner prints `search_completed=no` in that
case. The wall time is the runner's own measurement of `./pan` alone.

**Detector on (`DETECTOR=1`). Every case fails.**

| N | K | verdict | errors | states | depth of the error | wall |
|---|---|---|---|---|---|---|
| 2 | 1 | FAIL, assertion violated `rd` | 1 | 519 | 331 | 0.06 s |
| 2 | 3 | FAIL, assertion violated `rd` | 1 | 533 | 345 | 0.06 s |
| 2 | 10 | FAIL, assertion violated `rd` | 1 | 582 | 394 | 0.06 s |
| 2 | 100 | FAIL, assertion violated `rd` | 1 | 1212 | 1024 | 0.06 s |
| 3 | 1 | FAIL, assertion violated `rd` | 1 | 863 | 377 | 0.06 s |
| 3 | 3 | FAIL, assertion violated `rd` | 1 | 905 | 391 | 0.06 s |
| 3 | 10 | FAIL, assertion violated `rd` | 1 | 1052 | 440 | 0.06 s |
| 3 | 100 | FAIL, assertion violated `rd` | 1 | 2942 | 1070 | 0.06 s |
| 4 | 1 | FAIL, assertion violated `rd` | 1 | 872 | 752 | 0.06 s |
| 4 | 3 | FAIL, assertion violated `rd` | 1 | 886 | 766 | 0.06 s |
| 4 | 10 | FAIL, assertion violated `rd` | 1 | 935 | 815 | 0.06 s |
| 4 | 100 | FAIL, assertion violated `rd` | 1 | 1565 | 1445 | 0.06 s |

`K = 100` is the threshold the C code ships. It completes for every N in 0.06 s,
far inside the five-minute budget, so no smaller substitute was needed.

**Positive control (`DETECTOR=0`, main never aborts). Full search, 0 errors,
no invalid end state.**

| N | K | verdict | errors | invalid end states | states | search completed | wall |
|---|---|---|---|---|---|---|---|
| 2 | 3 | PASS | 0 | 0 | 2729 | yes | 0.06 s |
| 3 | 3 | PASS | 0 | 0 | 53793 | yes | 0.07 s |
| 4 | 3 | PASS | 0 | 0 | 1107433 | yes | 0.36 s |

The control also exercises the model invariants `nblocked <= nlive`,
`chan_len <= CAP` and `readyq <= N`, which `Main` asserts on every poll under
the same mutex the C reads those fields under. So the control is not vacuous:
it says the model has no assertion violation and no deadlock of its own.

The brief said that if the control fails, the model has a defect and the model
is what gets fixed. **It failed on the first attempt, and the model was
fixed.** The failure was not an invalid end state but
`error: max search depth too small` at pan's default `-m10000`, with
`errors: 0` printed beside it. The cause: `cond_signal` chose which waiter to
wake with a loop `sg_i = (sg_i + 1) % N` inside an `atomic`, and that loop can
run forever. The fix counts the waiters, chooses an ordinal in `0 .. n-1` with
a loop bounded by `N`, then walks to that waiter. The search now completes at
depth 317. The runner learned the lesson too: a truncated search that finds no
error now exits 2 as INCONCLUSIVE, not 0. `PAN_ARGS='-m50'` shows that path.

### Reading the counterexample

Files: `evidence/deadlock_detector_N2_K1.trail` and
`evidence/deadlock_detector_N2_K1.trace.txt`. This is the smallest failing
case, `N = 2` and `K = 1`. Goroutine 0 sends twice, goroutine 1 receives
twice, the buffer holds one value, and the program is correct Go: nothing about
it deadlocks.

Step numbers are the trace's own. 35 polls happen in all; 34 of them see a
running program and reset `asleep_streak` to 0.

| step | who | what |
|---|---|---|
| 4 | main | sets `main_in_wait = 1` and starts polling |
| 11 | g1 | a worker pops it, `readyq` 2 → 1 |
| 18 | g1 | state `G_RUNNING` |
| 29 | g1 | locks the channel mutex, first receive |
| 36 | g1 | `chan_len == 0`, so it must wait |
| 43 | g1 | state `G_WAITING` on `not_empty` |
| 50 | g1 | `goo_sched_block_begin`, `nblocked` 0 → 1 |
| 57 | g1 | `pthread_cond_wait` releases the channel mutex, g1 parks |
| 64 | g0 | a worker pops it, `readyq` 1 → 0 |
| 80 | g0 | locks the channel mutex, first send |
| 94 | g0 | `chan_len` 0 → 1 |
| 115 | g0 | `pthread_cond_signal(not_empty)`: g1 becomes `G_SIGNALLED`, `nblocked` STAYS 1 |
| 138 | g0 | unlocks the channel mutex |
| 148-163 | g1 | reacquires the mutex, `nblocked` 1 → 0, state `G_RUNNING` |
| 177-191 | g1 | takes the value, `chan_len` 1 → 0, broadcasts, unlocks |
| 201-229 | g1 | SECOND receive: locks, buffer empty, `G_WAITING`, `nblocked` 0 → 1, parks |
| 236-250 | g0 | SECOND send: locks, `chan_len` 0 → 1 |
| 271 | g0 | `pthread_cond_signal(not_empty)`: **g1 becomes `G_SIGNALLED`** |
| 294 | g0 | unlocks the channel mutex |
| 308 | g1 | reacquires the channel mutex, still `G_SIGNALLED`, `nblocked` still 1 |
| 315 | g0 | done with both sends, state `G_DONE` |
| 322 | g0 | `goo_goroutine_exit`, `nlive` 2 → 1 |
| 327 | main | poll 35: `nlive == 1`, `nblocked == 1`, `readyq == 0` → `all_asleep` |
| 329 | main | `asleep_streak` 0 → 1, which is `K` |
| 332 | main | `assert(rd)` FAILS |

The state at the abort:

```
gstate[0] = 4 (G_DONE)      gstate[1] = 3 (G_SIGNALLED)
nlive = 1   nblocked = 1    readyq = 0   chan_len = 1   chmx = 1
```

The one live goroutine, g1, was signalled at step 271, holds the channel mutex
at step 308, and its very next step is `goo_sched_block_end`, which would take
`blocked_goroutines` to 0. A value is waiting for it in the buffer.
`real_deadlock` is false because g1 is `G_SIGNALLED`, not `G_WAITING`. The C
code would print `fatal error: all goroutines are asleep - deadlock!` and
`exit(2)` on a program one step from finishing.

The same shape scales to any `K`: g1 stays `G_SIGNALLED` for as many of main's
polls as the schedule likes. The `K = 100` row is that schedule with 100 polls.

**Answer to the hypothesis: confirmed.** An abort with no deadlock is
reachable for every finite threshold tested, up to and including the 100 the C
code ships. The cause is exactly the one named: the model puts no bound on how
long a signalled goroutine stays unscheduled, and no finite count of polls can
outlast an unbounded delay.

### Fairness

The brief asked for `./pan -f`. Recorded verbatim:

- `./pan -f` on the `-DSAFETY` build **refuses to run**. pan prints
  `saw option -f` and then `-a,-l,-f -> are disabled by -DSAFETY`.
- Rebuilt without `-DSAFETY`, `./pan -f` prints
  `error: -f requires the use of -a or -l`.
- `./pan -a -f` prints
  `warning: no accept labels are defined, so option -a has no effect (ignored)`
  and then reports the same violation at the same depth:
  `pan:1: assertion violated rd (at depth 331)`, `errors: 1`, 519 states.

So the counterexample survives, but that survival is **not** evidence about
fairness: with `-a` ignored, `-f` had nothing to attach to.

The real answer is an argument, and it is short. Weak fairness constrains
INFINITE runs: a transition that stays continuously enabled must be taken
EVENTUALLY. An assertion violation is a finite prefix. No fairness assumption
can rule out a finite prefix, and the counterexample delays the signalled
goroutine by a finite number of main's steps. A weakly fair scheduler is
therefore free to produce it — weak fairness promises "eventually", never
"within K steps".

This is what makes the finding worse than a starvation bug, not better. A
counterexample that needed an unfair schedule would be a starvation of the
woken goroutine, and one could argue that a real OS scheduler does not starve
threads. This counterexample needs no starvation at all. It needs only a delay
longer than `K` polls, which every fair scheduler is entitled to produce.

The C code's threshold is a bet that the delay stays under about 50 ms. The
model has no clock, so it can neither confirm nor refute that bet. What it does
show is that the bet is a timing bet and not a proof, and that no larger
constant turns it into one. The `~1-in-3 across 8000 batches` false aborts
recorded in the `concurrency.c` comment for a threshold of 3 are that bet being
lost in practice.

### Instrument checks

A counterexample means nothing if the assertion is false everywhere. Two checks
on the instrument, both by inverting the claim to `assert(!rd)` on a scratch
copy and re-running:

| case | inverted assertion | reading |
|---|---|---|
| `N=2 K=1` | 0 errors, full search, 2988 states | for the BALANCED program, `real_deadlock` is NEVER true at the abort point. Every abort it can reach is false. |
| `N=3 K=1` | `assertion violated !(rd)` at depth 431, 395 states | for the UNBALANCED program, `real_deadlock` IS reachable at the abort point. |

The `N = 3` witness: goroutine 0 is a sender parked on `not_full` with
`chan_len == 1`, and goroutines 1 and 2 are `G_DONE`. No receiver is left, so
goroutine 0 waits forever. The detector aborts, and it is right to. So
`assert(rd)` is not a tautology, the detector does catch genuine deadlocks in
the model, and the `N = 2` counterexamples are real false positives rather than
an artefact of an unsatisfiable predicate.

`Main` asserts `_pid == N` and `Goroutine` asserts `_pid < N`, so a change in
the declaration order that scrambled the goroutine indices would be caught
rather than silently mis-indexing `gstate`.

### What this does NOT say

- It says nothing about wall-clock probability. The model has no clock, so it
  cannot say how often the abort happens on real hardware. The 100-poll
  threshold may well be enough in practice for the C stress tests that are its
  only remaining caller — `goo_scheduler_wait` stopped being emitted by
  generated code at P3.3.
- It does not exercise `close`, unbuffered channels, `select`, or the
  main-thread path of `goo_sched_block_begin`.
- pan's invalid-end-state check is close to vacuous here, because `Main` polls
  forever whenever a goroutine is live, so the system always has an executable
  transition. It still has one job: if `nlive` ever reached 0 while a goroutine
  was parked, `Main` would terminate and pan would report the stranded process.

### The fix the model points at

The detector counts a goroutine as blocked from `goo_sched_block_begin` until
`goo_sched_block_end`, and that interval covers a period in which the goroutine
is awake and running. Any threshold on a count of polls is a bet on how long
that period is. A detector that cannot be wrong needs the signal itself to
clear the block, not the woken thread's later return — for example, the
signaller decrements `blocked_goroutines` under the channel mutex at the same
moment it calls `pthread_cond_signal`, and the woken goroutine no longer calls
`goo_sched_block_end` on that path. Modelling that variant is not part of this
task; it is stated here so the next task has a target.
