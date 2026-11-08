# SPIN models

Formal models of Goo runtime protocols. Each model is checked with the
vendored SPIN (`scripts/spin_bootstrap.sh` prints the binary path).

Contents:

- `deadlock_detector.pml` — the runtime's deadlock detector. ONE file, TWO
  protocols, selected by `FIX`: `FIX=0` is the detector as it ships today,
  `FIX=1` is the corrected protocol.
- `run_deadlock_model.sh` — build and run that model.
- `evidence/` — committed counterexample trails and their readable traces.

---

## `deadlock_detector.pml` — the deadlock detector, as it is and as it should be

### One file, two protocols

`FIX` selects the protocol. A second file would drift from the first, so both
live here and share every other line of the model — the goroutines, the
channel, the poll loop, the claim.

| `FIX` | Protocol |
|---|---|
| `0` | **As it ships.** `blocked_goroutines++` at `goo_sched_block_begin`, `blocked_goroutines--` at `goo_sched_block_end`. Task 5 proved this aborts a live program for every finite threshold: the interval from `pthread_cond_signal` to the woken goroutine's `goo_sched_block_end` counts that goroutine as blocked, and nothing bounds that interval. |
| `1` | **Corrected.** The count is the size of a SET of parked goroutines, not a tally of `block_begin` calls. EVERY wake is a broadcast, and the broadcaster removes from the set — under the scheduler mutex, while it still holds the channel mutex — every waiter it wakes. `goo_sched_block_end` no longer touches the count. A woken goroutine is therefore not counted as blocked. |

The claim is the same for both. Only the accounting changes.

`FIX=0` is byte-for-byte task 5's protocol. Every `FIX=1` line sits inside an
`#if FIX`, including the `counted[N]` array, so the `FIX=0` state vector is the
one task 5 measured. Two checks hold that: the preprocessed model at `FIX=0` is
token-identical to the task 5 file, and every `FIX=0` row reproduces task 5's
state count exactly (below).

### What it models

The C under test, all at commit `f641564` (branch
`worktree-verify-tooling-next-steps`, off `build/bazel-phase4-probes`):

| C code | Model |
|---|---|
| `goo_scheduler_wait()` poll loop, `src/runtime/concurrency.c:143-201` | `proctype Main` |
| `goo_sched_block_begin()`, `concurrency.c:296-324` | `inline block_begin` |
| `goo_sched_block_end()`, `concurrency.c:327-335` | `inline block_end` |
| `scheduler_get_next_goroutine()`, `concurrency.c:435-446` | the `readyq--` step in `proctype Goroutine` |
| `goo_goroutine_exit()`, `concurrency.c:343-347` | the `gstate = G_DONE` then `nlive--` steps |
| `goo_chan_send()` buffered path, `src/runtime/channels.c:277-329` | `inline do_send` |
| `goo_chan_recv()` buffered path, `src/runtime/channels.c:422-479` | `inline do_recv` |
| `pthread_cond_signal(not_empty)`, `channels.c:325` | `inline cond_signal` under `FIX=0`; `inline cond_broadcast` under `FIX=1`, see ruling 1 |
| `pthread_cond_broadcast(not_full)`, `channels.c:475` | `inline cond_broadcast` |
| `pthread_cond_wait`, `channels.c:296` and `:436` | `inline cond_wait_park` |
| `goo_deadlock_abort()`, `src/runtime/deadlock.c:44-48` | `prog_exit = 1` and `goto stopped` |

Under `FIX=1`, `do_send` broadcasts `not_empty` instead of signalling it, and
`cond_broadcast` carries the new decrement. Those are the only places the two
protocols differ inside the channel code, and `cond_signal` is then unused.

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
four lines: one write at `concurrency.c:149`, one reset at `deadlock.c:17`, the
field declaration at `include/runtime.h:671`, and the name inside a comment at
`concurrency.c:170`. No line reads the field. It steers nothing.

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
   any time, as if `GOMAXPROCS >= N`. **This choice favours the
   counterexample, not the detector.** A large pool empties the ready queue,
   and an empty ready queue is one of the three terms of `all_asleep`, so it
   makes an abort EASIER. A pool smaller than N would leave the ready queue
   non-empty, which RESETS `asleep_streak` and would make an abort HARDER.

   The defence is not that the abstraction is conservative. It is that the
   large pool is the shipped default: `goo_default_thread_count()`
   (`src/runtime/concurrency.c:63-80`) returns `GOMAXPROCS` when it parses,
   otherwise the online CPU count, clamped to `GOO_MAX_OS_THREADS`, which is
   16. For N of 2 to 4 the default pool is therefore larger than N on any
   machine with 4 or more cores, and never smaller than N unless the operator
   sets `GOMAXPROCS` below it. The model and its counterexample hold for the
   configuration the runtime actually uses.

   A reader who runs with `GOMAXPROCS=1` is outside the model. The
   counterexample says nothing about that case.
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
| `FIX` | 0 the protocol as it ships, 1 the corrected protocol | 0 |
| `CAP` | channel buffer capacity | 1 |
| `OPS` | channel operations per goroutine | 2 |

Three more parameters exist only to give the `FIX=1` proof teeth, and only
`FIX=1` reads them. Each is ONE change, so a reader can see which change
each error is attributable to.

| Name | Meaning | Default |
|---|---|---|
| `DOUBLE_DEC` | `block_end` decrements as well as the broadcaster | 0 |
| `BCAST_ONE` | a broadcast decrements by ONE, not by the number of waiters it woke | 0 |
| `SPURIOUS` | `pthread_cond_wait` may return with no signal, which POSIX permits | 0 |
| `END_UNREG` | `block_end` unregisters this goroutine IF it is still registered | 0 |

Goroutine `i` sends when `i` is even and receives when `i` is odd. So `N = 2`
and `N = 4` balance the sends against the receives, and `N = 3` does not: it
has two senders and one receiver, so a genuine deadlock is reachable there.

### Commands

```
./tests/models/run_deadlock_model.sh N K [DETECTOR] [FIX]
```

`DETECTOR` defaults to 1 and `FIX` defaults to 0, so every command written
before `FIX` existed still means what it meant. The verdict line prints both,
and prints the whole `-D` set as `defs='...'`, so no run can be mistaken for a
different one.

The four teeth and spurious-wakeup parameters have no positional slot. Pass
them in `MODEL_DEFS`, which is appended to the define set and printed:

```
MODEL_DEFS='-DDOUBLE_DEC=1' ./tests/models/run_deadlock_model.sh 2 1 1 1
```

Exit status:

- **0** — pan completed the search and reported 0 errors.
- **1** — pan reported an error.
- **2** — the arguments are wrong, spin/cc/pan failed to give a verdict, or the
  run was **INCONCLUSIVE**: pan found no error but did not finish the search,
  or it explored 0 states. Such a run proves nothing, so it is never a pass.

`KEEP_DIR=<dir>` copies the model, `pan.log`, any `.trail` and a `spin -t -p`
trace into `<dir>`. `PAN_ARGS` passes extra flags to `./pan`. `CC` may carry
flags, the way `make` treats it, so `CC='cc -DMEMLIM=1'` works; the verdict
line prints the `CC` value as well as the compiler's own version, or a
wrapper's flags would be invisible. The work
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

To replay a committed trail, use the `-D` set that trail was recorded with.
Each is listed beside its file:

| Trail | `-D` set |
|---|---|
| `evidence/deadlock_detector_N2_K1.trail` | `-DN=2 -DK=1 -DDETECTOR=1 -DFIX=0` |
| `evidence/deadlock_detector_FIX1_N3_K1_bcast_one.trail` | `-DN=3 -DK=1 -DDETECTOR=1 -DFIX=1 -DBCAST_ONE=1` |
| `evidence/deadlock_detector_FIX1_N2_spurious.trail` | `-DN=2 -DK=3 -DDETECTOR=0 -DFIX=1 -DSPURIOUS=1` |

```
cp tests/models/deadlock_detector.pml "$WORK/"
cp tests/models/evidence/deadlock_detector_N2_K1.trail \
   "$WORK/deadlock_detector.pml.trail"
cd "$WORK" && "$SPIN" -t -p -DN=2 -DK=1 -DDETECTOR=1 -DFIX=0 deadlock_detector.pml
```

The task 5 trail was recorded before `FIX` existed, and it still replays
against this model at `FIX=0`, to the same 332 steps and the same end state.
That is the second check that `FIX=0` did not move.

### Results

Instrument: Spin Version 6.5.2 (6 December 2019), vendored, built by
`scripts/spin_bootstrap.sh`; `cc (GCC) 16.2.1 20260819 (Red Hat 16.2.1-2)`
with `-O2 -DSAFETY`; Linux 7.1.12-200.fc44.x86_64, 32 cores.
The `FIX=0` rows were taken 2026-09-02 on branch `build/bazel-phase4-probes`
at `9311080`, and re-taken unchanged at `f641564`. The `FIX=1` rows were taken
2026-09-02 at `f641564` on branch `worktree-verify-tooling-next-steps`, on the
same machine with the same instrument.

`errors` and `states` come from pan's own summary. For a FAIL, pan stops at
the first error, so the state count is what it explored before the error and
not the whole state space — the runner prints `search_completed=no` in that
case. The wall time is the runner's own measurement of `./pan` alone.

Every row below is asked for a FULL state space search: `-DSAFETY`, no
`-DBITSTATE`, no `PAN_ARGS`, so no depth limit below pan's default 10000 and no
memory bound, and pan's own header reads
`Full statespace search for: ... assertion violations +`. Each PASS row got
one: it printed `search_completed=yes`, and the runner refuses to call a
truncated search a PASS, so a PASS explored everything. A FAIL row did not, and
cannot: pan stops at the first error by design, and a counterexample is a
witness rather than a count.

## `FIX=0` — the detector as it ships

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
depth 317.

The runner learned the lesson too, in two rounds. A truncated search that
finds no error exits 2 as INCONCLUSIVE, not 0. The first version of that rule
named the causes it knew, and the 2026-09-02 review then found a cause it did
not know: a pan built with `-DMEMLIM=1` prints `pan: reached -DMEMLIM bound`,
explores 0 states, and the runner printed PASS with `states=0` and exited 0.
The rule is now on the OUTCOME, not the cause: `errors == 0` together with an
unfinished search, or 0 states explored, is INCONCLUSIVE whatever pan says.
The unfinished test itself carries a `^error:` catch-all, because pan does NOT
print `Search not completed` for the depth limit.

The two negative controls for that rule:

```
$ PAN_ARGS='-m50' ./tests/models/run_deadlock_model.sh 2 3 0
run_deadlock_model: INCONCLUSIVE model=tests/models/deadlock_detector.pml N=2 K=3 DETECTOR=0 FIX=0 errors=0 states=797 search_completed=no defs='-DN=2 -DK=3 -DDETECTOR=0 -DFIX=0 ' -- pan did not complete the search and found no error, so it proves nothing
run_deadlock_model:   pan said: error: max search depth too small
rc=2

$ CC='cc -DMEMLIM=1' ./tests/models/run_deadlock_model.sh 4 3 0
run_deadlock_model: INCONCLUSIVE model=tests/models/deadlock_detector.pml N=4 K=3 DETECTOR=0 FIX=0 errors=0 states=0 search_completed=no defs='-DN=4 -DK=3 -DDETECTOR=0 -DFIX=0 ' -- pan did not complete the search and found no error, so it proves nothing
run_deadlock_model:   pan said: pan: reached -DMEMLIM bound
rc=2
```

## `FIX=1` — the corrected protocol

### What changes

Three edits, and nothing else:

1. **`block_begin` registers, and registering is idempotent.** `counted[i]` is
   goroutine `i`'s membership of the blocked set. `nblocked` is the size of
   that set. A second `block_begin` with the bit already set adds nothing.
2. **Every wake is a BROADCAST.** `do_send` broadcasts `not_empty` under
   `FIX=1`, where the C ships a `pthread_cond_signal` (`channels.c:325`, and
   `:356` and `:543` on the paths this model does not carry). `do_recv`
   already broadcast `not_full`, as the C does.
3. **The broadcaster unregisters.** It clears the bit of every waiter it wakes
   and decrements by THAT NUMBER, under the scheduler mutex, taken while the
   channel mutex is already held.
4. **`block_end` no longer touches the count.** It asserts
   `counted[_pid] == 0` instead, which turns "it does not need to" from an
   assumption into a theorem of the model.

Change 2 is not decoration, and it is the 2026-09-02 review's finding 1.
`pthread_cond_signal` gives the caller no way to learn WHICH waiter the OS
woke. A signal paired with "clear the bit of the one waiter this will wake"
assumes an answer POSIX does not give: if the C clears X's bit and the OS wakes
Y, then Y is awake and still registered, and that is the shipped defect again.
A broadcast removes the question, because the set woken is then the whole
waiter set, which is exactly the set the broadcaster unregisters. The model
proves the broadcast protocol, so the broadcast protocol is what task 8 must
build. The cost is in "The C-level fix" below.

The lock order does not change. `block_begin` already takes the scheduler mutex
while holding the channel mutex (`concurrency.c:304` inside a caller that holds
`ch->mutex`), and the wake sites are inside the same channel mutex region, so
the new decrement is channel-then-scheduler exactly like the old increment. In
the model the scheduler-mutex acquisition is the FIRST statement of the
`cond_broadcast` sequence, which makes it that sequence's guard: a busy mutex
leaves the sequence not enabled rather than half executed.

### One abstraction the fix introduces, and why it is safe

The model performs the decrement and the state change to `G_SIGNALLED` in one
indivisible step, for every waiter the broadcast wakes. Real C cannot: it must lock the scheduler mutex, decrement,
unlock, then call `pthread_cond_broadcast`, all under the channel mutex. That
leaves a window in which a waiter is uncounted but still parked.

That window can only make `blocked_goroutines` LOWER than the model's value,
never higher, so `all_asleep` is FALSE throughout it. The reason is direct: the
waiter the broadcaster just decremented is itself LIVE and PARKED and now
UNCOUNTED, so `blocked_goroutines < num_goroutines` and the equality
`all_asleep` needs cannot hold. That argument does not depend on who the
broadcaster is, which matters because main can be the broadcaster too. A poll
that lands in the window therefore RESETS `asleep_streak` and removes an abort;
it never adds one. The set of states at which an abort can happen is the same
in both, so a proof over the model covers the C.

**The other order is not safe.** `pthread_cond_broadcast` first, then
decrement, recreates the very window task 5 found — narrower, but still a
window in which a woken goroutine is counted as blocked. The decrement must
precede the broadcast.

### The claim, proved. `K = 1`, full exhaustive search, 0 errors.

`K = 1` is the hardest case: one single poll of all-parked state is enough to
abort. Any larger `K` gives the model more chances to reset the streak, so it
can only remove aborts. Proving `K = 1` proves every larger threshold.

| N | K | verdict | errors | invalid end states | states | search completed | depth | wall |
|---|---|---|---|---|---|---|---|---|
| 2 | 1 | PASS | 0 | 0 | 2274 | yes | 365 | 0.07 s |
| 3 | 1 | PASS | 0 | 0 | 44866 | yes | 544 | 0.07 s |
| 4 | 1 | PASS | 0 | 0 | 998166 | yes | 872 | 0.33 s |

The state counts are exact and repeat to the digit. The wall times move by
about 0.01 s between runs of the same row, because the runner times the whole
of `./pan` including process start.

Only N = 4 moved when `do_send` became a broadcast (finding 1): 1017218 states
became 998166. N = 2 and N = 3 have ONE receiver each, so `not_empty` never
holds two waiters, and a broadcast there is the same instruction as a signal.
That is the same shape as the `BCAST_ONE` split below, and it carries the same
warning: a change to a wake primitive shows nothing until two waiters can share
one condvar.

**Positive control, `FIX=1` with the detector off.** The model has no assertion
violation and no deadlock of its own, so a PASS above is not the model being
silent.

| N | K | verdict | errors | invalid end states | states | search completed | wall |
|---|---|---|---|---|---|---|---|
| 2 | 3 | PASS | 0 | 0 | 1731 | yes | 0.06 s |
| 3 | 3 | PASS | 0 | 0 | 34535 | yes | 0.07 s |
| 4 | 3 | PASS | 0 | 0 | 771876 | yes | 0.29 s |

### Is the PASS vacuous? Partly, and pan says exactly where.

A PASS on `assert(rd)` can mean two different things, and the difference
matters. pan's `unreached in proctype Main` report and the inverted-claim
control agree on which is which:

| N | is the abort reached? | pan's unreached report | inverted `assert(!rd)` | reading |
|---|---|---|---|---|
| 2 | NO | `assert(rd)` unreached | 0 errors, full search, 2274 states | the balanced program has no deadlock, and `FIX=1` never aborts it. The false abort task 5 found is GONE. |
| 3 | YES | `(0 of 86 states)` unreached | `assertion violated !(rd)` at depth 396, 395 states | the abort IS reached, and `rd` holds every time it is. A real proof. |
| 4 | NO | `assert(rd)` unreached | 0 errors, full search, 998166 states | as N = 2. Two senders and two receivers cannot deadlock here. |

So the load-bearing row is `N = 3`, which has two senders and one receiver and
therefore a genuine deadlock: a sender parked on `not_full` with no receiver
left. `FIX=1` aborts there, and every abort it reaches is a true deadlock.
`N = 2` and `N = 4` say something different but not weaker: under `FIX=0` those
same programs WERE aborted, and under `FIX=1` they are not aborted at all.

Under `FIX=0` the same two instruments say the opposite for `N = 2`: the abort
is reached (task 5's counterexample) and `rd` is false there.

### Teeth. Each mutant is ONE change and each must fail.

| mutant | N | K | verdict | what broke | states | depth |
|---|---|---|---|---|---|---|
| none — `FIX=0`, the shipped protocol | 2 | 1 | FAIL | `assertion violated rd` | 519 | 331 |
| none — `FIX=0`, the shipped protocol | 3 | 1 | FAIL | `assertion violated rd` | 863 | 377 |
| none — `FIX=0`, the shipped protocol | 4 | 1 | FAIL | `assertion violated rd` | 872 | 752 |
| `DOUBLE_DEC=1` | 2 | 1 | FAIL | `assertion violated (nblocked>0)` | 139 | 139 |
| `DOUBLE_DEC=1` | 3 | 1 | FAIL | `assertion violated (nblocked>0)` | 183 | 183 |
| `BCAST_ONE=1` | 2 | 1 | PASS | nothing — see below | 2274 | 365 |
| `BCAST_ONE=1` | 3 | 1 | FAIL | `assertion violated rd` | 8201 | 515 |
| `BCAST_ONE=1` | 4 | 1 | FAIL | `assertion violated rd` | 24811 | 642 |
| `SPURIOUS=1` | 2 | 3 | FAIL | `assertion violated (counted[_pid]==0)` | 63 | 63 |
| `SPURIOUS=1` | 3 | 3 | FAIL | `assertion violated (counted[_pid]==0)` | 92 | 92 |

`DOUBLE_DEC=1` is the mutant the task brief named: the wake-time decrement put
back beside the signal-time one. Both fire for one wake, the count goes below
zero, and `assert(nblocked > 0)` inside `block_end` catches it at N = 2 in 139
states. The count is a `byte` in Promela, so without that assertion it would
wrap to 255 and the failure would surface later and less clearly.

### Broadcast, and why the decrement must be by the number woken

`goo_chan_recv` already broadcasts `not_full` rather than signalling it
(`channels.c:475` on the buffered path, `channels.c:505` on the rendezvous
path, `channels.c:574` in `goo_chan_try_recv`). The reason is in the C comment
at `channels.c:466-473`: two predicate classes share that condvar, and a single
signal can wake the wrong class. `goo_chan_close` broadcasts both condvars
(`channels.c:97-98`). Under `FIX=1` the send side broadcasts `not_empty` too,
for the different reason in "What changes" above, so a broadcast is the ONLY
wake primitive the corrected protocol uses.

**The C's broadcast sites CAN wake more than one waiter**, so the decrement
must be by the number woken. In the model, goroutine `i` sends when `i` is even
and receives when `i` is odd, so `N = 3` and `N = 4` each have two senders, and
both can be parked on `not_full` at once with `CAP = 1`. A receiver's broadcast
then wakes two.

`BCAST_ONE=1` is the mutant that decrements by one anyway. It shows exactly
that split:

- **N = 2 PASSES**, with the same 2274 states as the unmutated run. There is
  one sender and one receiver, so neither condvar ever holds two waiters, and
  "decrement by one" and "decrement by the number woken" are the same
  instruction. A reader who tested only `N = 2` would have concluded the
  decrement amount does not matter.
- **N = 3 and N = 4 FAIL**, `assertion violated rd`, in 8201 and 24811 states.
  The witness is committed at
  `evidence/deadlock_detector_FIX1_N3_K1_bcast_one.trail`. Its end state:

```
gstate[0] = 3 (G_SIGNALLED)   gstate[1] = 4   gstate[2] = 4 (both G_DONE)
nlive = 1   nblocked = 1   readyq = 0   chan_len = 1
counted[0] = 0   counted[1] = 0   counted[2] = 0
```

`nblocked` is 1 while the set it is supposed to count is EMPTY. One broadcast
woke two waiters and gave back one slot. That surplus made `nblocked == nlive`
true for a goroutine that had already been signalled, which is task 5's bug
reintroduced through the arithmetic instead of through the timing.

### Spurious wakeups

**What the C does.** Every `pthread_cond_wait` in `channels.c` sits inside a
`while` loop that re-checks its predicate and waits again: `channels.c:280`
(buffered send), `:336` and `:359` (rendezvous send), `:425` (buffered recv),
`:484` (rendezvous recv). So a spurious return re-checks and re-parks, which is
the correct POSIX idiom and is already in place.

**What the model assumes.** The proof rows above set `SPURIOUS=0`: a return
from the wait happens only after a broadcast. `SPURIOUS=1` adds the second
branch — a return with no broadcast, at any time.

#### What the model actually proves here, and what it does not

The 2026-09-02 review's finding 3. An earlier draft of this section claimed the
`SPURIOUS=1` trail showed a COUNT LEAK. It does not, and the correction
matters, because the two claims need different evidence.

What the trail shows is the failure of ONE assertion:

```
$ MODEL_DEFS='-DSPURIOUS=1' ./tests/models/run_deadlock_model.sh 2 3 0 1
... FAIL ... errors=1 ... states=63 ...
pan:1: assertion violated (counted[_pid]==0) (at depth 63)
```

That assertion is the "`block_end` does not need to touch the count" theorem
inside `block_end`. Under `SPURIOUS=0` it holds on every reachable path: the
broadcaster always cleared the bit first. Under `SPURIOUS=1` it does not, and
that is a correct and useful result — the theorem the corrected protocol leans
on is only true when every return from a wait came from a broadcast.

It is NOT a leak. The committed witness
(`evidence/deadlock_detector_FIX1_N2_spurious.trail`) ends with `chan_len = 0`
and `counted[1] = 1`: goroutine 1 re-checks its predicate, finds it still true,
and parks again, so nothing is lost. Replacing that assertion with `skip` and
searching the whole state space finds no other complaint:

```
$ spin -DN=2 -DK=1 -DDETECTOR=1 -DFIX=1 -DSPURIOUS=1 -a deadlock_detector.pml   # assertion -> skip
$ cc -O2 -DSAFETY -o pan pan.c && ./pan
State-vector 44 byte, depth reached 587, errors: 0
     3515 states, stored
```

**Why the model cannot show the leak.** It has no condition-variable QUEUE. Its
broadcaster scans for `gstate[i] == G_WAITING`, and a model goroutine that
returns spuriously stays `G_WAITING` until after `block_end`, so a later
broadcast still finds it and still clears its bit. A real condvar drops a
thread from its queue the moment `pthread_cond_wait` returns, spuriously or
not, and a later broadcast never reaches it. Modelling that needs a wait queue
separate from the goroutine state, which this model does not have.

#### The leak is a C-level argument

Take the buffered receive, `channels.c:425-446`, with the corrected protocol
and with `goo_sched_block_end` made a no-op:

1. The thread parks. `goo_sched_block_begin()` (`:435`) sets its bit and makes
   `blocked_goroutines` 1 higher. `pthread_cond_wait` (`:436`) puts it on
   `not_empty`'s queue.
2. `pthread_cond_wait` returns with no broadcast. POSIX has now taken the
   thread OFF the queue. `goo_sched_block_end()` (`:437`) does nothing, so the
   bit stays set.
3. The `while` at `:425` re-checks `ch->length == 0 && !ch->closed`.
   - **Predicate still true** — the thread calls `goo_sched_block_begin()`
     again. That call does nothing, because the bit is already set, and
     `pthread_cond_wait` puts the thread back on the queue. Consistent, and
     this is the case the model's witness shows.
   - **Predicate now false** — some sender filled the buffer while this thread
     was off the queue. The thread consumes the value and LEAVES the loop with
     its bit still set. It is on no queue, so no future broadcast can ever
     reach it and clear the bit. `blocked_goroutines` is permanently 1 too
     high. **That is the leak.**

The leak is not only a missed deadlock. With two goroutines, one leaked and
running and one genuinely parked, `blocked_goroutines == num_goroutines` holds
while a goroutine runs, and the detector aborts a live program — the shipped
defect made permanent instead of transient.

The same three steps apply at `channels.c:280-306` (buffered send), and at
`:336-340`, `:359-363` and `:484-488` on the rendezvous paths.

**The correction, and it stays.** `goo_sched_block_end` must unregister this
goroutine IF it is still registered. That is a conditional removal, not a plain
decrement, so it can never drive the count below zero, and it does nothing
whenever the broadcaster got there first. `END_UNREG=1` models it. The model
cannot prove the correction is NECESSARY, for the queue reason above, but it
does show the correction is HARMLESS — it changes no verdict, and the
accounting stays exact under `SPURIOUS=1`:

| N | K | `MODEL_DEFS` | verdict | states | search completed | wall |
|---|---|---|---|---|---|---|
| 2 | 3 | `-DSPURIOUS=1 -DEND_UNREG=1` | PASS | 2449 | yes | 0.06 s |
| 3 | 3 | `-DSPURIOUS=1 -DEND_UNREG=1` | PASS | 56406 | yes | 0.08 s |
| 4 | 3 | `-DSPURIOUS=1 -DEND_UNREG=1` | PASS | 1345089 | yes | 0.46 s |

Those three rows run with `DETECTOR=0`, so they check the ACCOUNTING only:
`nblocked <= nlive` on every poll, never-below-zero at every decrement, and
`counted[i] == 1` at every unregister. They do not check `assert(rd)`.

**Why `assert(rd)` is not checked under `SPURIOUS=1`.** It fails at N = 3, and
the failure is a limit of the predicate, not of the protocol:

```
$ MODEL_DEFS='-DSPURIOUS=1 -DEND_UNREG=1' ./tests/models/run_deadlock_model.sh 3 1 1 1
... FAIL ... states=602 ... pan:1: assertion violated rd (at depth 546)
end state: gstate[0] = 1 (G_RUNNING)  gstate[1] = gstate[2] = 4 (G_DONE)
           nlive = 1  nblocked = 0  readyq = 0  chan_len = 1  counted[] all 0
```

Goroutine 0 is a sender parked on `not_full` with a full buffer and no receiver
left — a genuine deadlock, and the abort is CORRECT. `main` took its snapshot
while goroutine 0 was parked and counted, then released the scheduler mutex.
Goroutine 0 woke spuriously in that gap, so by the time `rd` is evaluated it
reads `G_RUNNING`. It will re-check `ch->length >= ch->capacity`, find it still
true, and park again forever.

**That gap is REAL in the C, not an artefact of the model.** The three
conjuncts of `all_asleep` are read under the scheduler mutex
(`concurrency.c:180-183`), the mutex is released at `concurrency.c:184`, and
`goo_deadlock_abort()` runs at `concurrency.c:194-196` — outside it. The model
does the same thing in the same order (`deadlock_detector.pml`, the poll
`atomic` and the `assert(rd)` after it). Task 8 must close the gap; the
instruction is in "The C-level fix" below. Under `SPURIOUS=0` the gap is
harmless, because only a broadcaster can move a parked goroutine and no
broadcaster can run when every live goroutine is parked. Under `SPURIOUS=1` it
is not, and a state predicate cannot say "running, but about to park again".

N = 2 and N = 4 pass `assert(rd)` even with `SPURIOUS=1 END_UNREG=1` (3182 and
1704404 states, both full searches), because those programs never reach the
abort at all.

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
copy and re-running. Both rows below are `FIX=0`; the `FIX=1` inverted rows are
in "Is the PASS vacuous?" above:

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

Three more checks were added when `FIX` was added:

1. **`FIX=0` did not move.** `cpp -P -DN=2 -DK=1 -DDETECTOR=1 -DFIX=0` on this
   file and `cpp -P -DN=2 -DK=1 -DDETECTOR=1` on the task 5 file give the same
   token stream (`tr -s ' \n\t' ' '` on both, then `diff`, empty). Every
   `FIX=0` row also reproduces task 5's state count to the digit: 519, 863,
   872 for the detector-on rows at `K = 1`, and 2729, 53793, 1107433 for the
   controls. The task 5 trail replays against this model at `FIX=0` to the
   same 332 steps.
2. **The `-O2` build is not lying.** `spin -DFIX=1 -DN=3` makes gcc emit two
   `-Wstringop-overflow` warnings on `pan.b`'s state-restore path, which the
   `FIX=0` build does not. pan's own `Boundcheck` macro is compiled in, so a
   real out-of-range index would be an error in the report, and the report has
   none. Rebuilding the same `pan.c` at `-O0` gives the identical verdict and
   the identical 44866 states, so the warning is gcc's range inference on a
   path the bounds check guards, not a miscompilation.
3. **The `FIX=1` PASS is not silence.** pan's `unreached in proctype Main`
   report names `assert(rd)` when the abort is never reached, and the
   inverted-claim control agrees with it row for row. See "Is the PASS
   vacuous?" above.

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
- `FIX=1` is proved for the buffered path with one channel and no `close`. The
  `goo_chan_close` broadcast sites (`channels.c:97-98`) and the rendezvous
  paths carry the same decrement in the C-level fix, and the model does not
  cover them. A second sender class on `not_full` is exactly the case
  `channels.c:466-473` warns about, and it is the case `BCAST_ONE` shows is
  arithmetic-sensitive, so those sites need the same care and no less.
- With `SPURIOUS=1`, `assert(rd)` is not proved. The accounting is, for
  N = 2, 3 and 4. See "Spurious wakeups" for the reason and for the witness,
  which is a TRUE deadlock read at the wrong instant.
- **It cannot show the spurious-wakeup count leak, and it does not.** The model
  has no condition-variable queue, so its broadcaster still finds a spuriously
  woken goroutine and still clears its bit. The leak that makes ruling 3's
  conditional unregister necessary is a C-level argument, set out with its C
  lines under "The leak is a C-level argument". Do not cite a model row as
  evidence for it.
- It says nothing about the throughput cost of ruling 1's broadcast. The model
  has no clock and counts no wakeups. The thundering-herd argument in the
  C-level fix is an argument, not a measurement.

### The C-level fix

This section is the input to task 8. Line numbers are from this branch at
`f641564`. Nothing in `src/` was changed to write it.

#### The one idea

`blocked_goroutines` is a tally of `goo_sched_block_begin` calls today. It must
become the SIZE OF A SET: the set of goroutines that are parked and that no
waker has released. Everything below follows from that one change.

Three rulings come out of the 2026-09-02 review and are not negotiable inside
this design. Ruling 1: every wake becomes a broadcast. Ruling 2: the waker
needs a waiter LIST, not a waiter count. Ruling 3: the poll decides and aborts
with the scheduler mutex held. Each has its own subsection below.

#### Ruling 1: every wake becomes a broadcast

`pthread_cond_signal` gives the caller NO WAY to learn which waiter the OS
woke. So a signal site cannot pair itself with "clear the bit of the waiter
this will wake": if it clears X's bit and the OS wakes Y, then Y is awake and
still registered, and that is exactly the shipped defect. There is no version
of the fix that keeps `pthread_cond_signal` and stays correct.

**The three `pthread_cond_signal` calls therefore become
`pthread_cond_broadcast`:** `channels.c:325` (buffered send), `:356`
(rendezvous send), `:543` (`goo_chan_try_send`). With that change the set the
OS wakes is the whole waiter set, which is exactly the set the broadcaster
unregisters, and that is the protocol the model proves.

**The cost is a thundering herd.** A broadcast on a channel with W waiting
receivers wakes all W. Each one reacquires `ch->mutex` in turn, re-checks its
predicate, and all but one find the buffer empty again and park again. That is
O(W) wakeups and O(W) mutex handoffs for one delivered value, where a signal
would have cost one. On a channel with a handful of waiters this is noise. On a
fan-in with hundreds of receivers it is not, and it is a real regression in
throughput, not only in theory.

Three things make it acceptable here, and task 8 should record all three:

- The receive side ALREADY broadcasts `not_full` (`channels.c:475`, `:505`,
  `:574`), for the lost-wakeup reason at `channels.c:466-473`. Half the channel
  traffic already pays this.
- The alternative is a detector that can abort a live program, which is what
  task 5 measured.
- If the herd ever costs too much, the way out is NOT to go back to
  `pthread_cond_signal`. It is to stop using a condvar the caller cannot
  interrogate: a hand-rolled wait queue, where the waker chooses the thread and
  therefore knows which bit to clear. That is a larger change and it is not
  this fix.

#### Ruling 2: the per-condvar waiter LIST is necessary, not a choice

The set needs a membership bit, because a broadcaster must clear the bit of
each waiter it wakes. Two pieces of state, and BOTH are necessary:

- **One bit per goroutine**, for example `int counted_blocked` beside
  `waiting_on_channel` and `waiting_for_send`, which are already in
  `goo_goroutine_t` at `include/runtime.h:771-772`. This makes registering and
  unregistering idempotent.
- **One waiter LIST per condition variable**, held under the channel mutex. The
  channel already carries `not_full` and `not_empty`; each needs a list of the
  goroutines currently waiting on it.

**A bare `int n_waiters` count is NOT enough**, and this is the 2026-09-02
review's finding 2. With a count, a broadcast can decrement
`blocked_goroutines` by `n` but it cannot clear any bit, because it does not
know WHOSE bits to clear. Every woken thread then reaches ruling 3's
conditional unregister in `goo_sched_block_end`, still finds its own bit set,
and decrements again. The count falls by `2n` for `n` wakeups. That is the
`DOUBLE_DEC` mutant exactly, and the model fails it at N = 2 in 139 states.

The identities are what the broadcaster needs, so the list is what it must
have. `self->waiting_on_channel` and `self->waiting_for_send`
(`include/runtime.h:771-772`) already name the two predicate classes, so the
list has an obvious shape: link the goroutines through a `next_waiter` field
and hang one list head off each condvar.

#### Which functions change

**`goo_sched_block_begin`, `concurrency.c:296-324`.** The goroutine branch is
`concurrency.c:305-313`. Line `:302` already calls `goo_current_goroutine()`
but keeps only a truth value; it must keep the pointer instead. Line `:310`,
`g_scheduler->deadlock_detector.blocked_goroutines++`, then becomes a guarded
increment:

```
goo_goroutine_t* self = goo_current_goroutine();   /* was: int is_goroutine */
...
if (!self->counted_blocked) {
    self->counted_blocked = 1;
    g_scheduler->deadlock_detector.blocked_goroutines++;
}
```

The mutex around it (`:304` and `:311`) does not move. The main-thread branch
at `:315-323` is untouched, and `self == NULL` still selects it.

**`goo_sched_block_end`, `concurrency.c:327-335`.** Lines `:331-333` are the
old wake-time decrement. They become a CONDITIONAL unregister, not a plain
decrement and not a deletion. `self` is the pointer `:329` already fetches from
`goo_current_goroutine()`, which today it only tests against NULL:

```
goo_goroutine_t* self = goo_current_goroutine();
if (!self) return;                       /* main is not counted; this is :329 */
goo_mutex_lock(g_scheduler->scheduler_mutex);
if (self->counted_blocked) {
    self->counted_blocked = 0;
    g_scheduler->deadlock_detector.blocked_goroutines--;
}
goo_mutex_unlock(g_scheduler->scheduler_mutex);
```

On the normal path the broadcaster has already cleared the bit, so this is a
no-op; the model proves that with `assert(counted[_pid] == 0)` and a full
search at `SPURIOUS=0`. It is NOT dead code. It is what stops a spurious return
from leaking a set member — the argument, with its C lines, is in "The leak is
a C-level argument" above. The model cannot show that leak, because it has no
condvar queue; the argument is a C-level one and it stands on the C. A plain
unguarded decrement here is the `DOUBLE_DEC` mutant, and it drives the count
below zero.

**The eight `pthread_cond_broadcast` calls after ruling 1.** `channels.c:325`,
`:356` and `:543`, which ruling 1 converts from signals, plus `:475`, `:505`,
`:574`, and `:97` and `:98` in `goo_chan_close`. Each takes the same shape,
with the channel mutex already held:

```
goo_mutex_lock(g_scheduler->scheduler_mutex);
for (goo_goroutine_t* w = ch->not_empty_waiters; w; w = w->next_waiter) {
    if (w->counted_blocked) {
        w->counted_blocked = 0;
        g_scheduler->deadlock_detector.blocked_goroutines--;
    }
}
goo_mutex_unlock(g_scheduler->scheduler_mutex);
pthread_cond_broadcast(&ch->not_empty->cond);
```

`goo_chan_close` walks both lists, one for each condvar.

**Lock order.** Channel mutex, then scheduler mutex. That is already the order
`goo_sched_block_begin` uses — `channels.c:295` calls it while holding
`ch->mutex`, and it takes the scheduler mutex at `concurrency.c:304`. The new
decrement sits inside the same channel mutex region, so it adds no new order.

**Order within the broadcast site.** Decrement FIRST, then call
`pthread_cond_broadcast`. The model's argument is in "One abstraction the fix
introduces" above: decrement-then-broadcast leaves a window in which a waiter
is uncounted but parked, and that waiter is itself live, so
`blocked_goroutines < num_goroutines` and `all_asleep` cannot hold. It can only
remove an abort. Broadcast-then-decrement leaves a window in which a woken
goroutine is still counted, which is the original defect at a smaller scale.

#### Ruling 3: decide AND abort under the scheduler mutex

Today the poll reads its three conjuncts under the scheduler mutex
(`concurrency.c:180-183`), releases the mutex at `concurrency.c:184`, and calls
`goo_deadlock_abort()` at `concurrency.c:194-196` — outside it. The state can
therefore change between the snapshot and the abort, and the model shows a case
where it does: see "Why `assert(rd)` is not checked under `SPURIOUS=1`" above.
This window is REAL in the C, not an artefact of the model.

**Task 8 must decide and abort with the scheduler mutex held.** The cost is
nothing: `goo_deadlock_abort()` is one `fprintf`, one `fflush` and `exit(2)`
(`deadlock.c:44-48`). It takes no lock, so it cannot deadlock against the one
being held, and the process is about to end, so holding a mutex through
`exit(2)` costs nothing that matters.

Concretely: move the `asleep_streak` update and the `asleep_streak >= K` test
inside the critical section that begins at `concurrency.c:175`, and call
`goo_deadlock_abort()` there, before the unlock. The `done || stopped` break at
`:197-199` can stay outside, because it is not a claim about a frozen state.

#### The 100-poll threshold

The model proves `K = 1` is enough for the ACCOUNTING: with the corrected
protocol, one poll of all-parked state is already a real deadlock, and a larger
`K` cannot make it more real. The streak buys nothing that the corrected count
does not already give.

The recommendation is to **keep a small `K`, and drop the 100**. Two reasons,
and they are not about correctness:

- The poll itself is a sleep loop (`goo_platform_sleep_ns(500000)` at
  `concurrency.c:173`). A threshold of 1 aborts after the first 0.5 ms poll
  that sees an all-parked state, which is fine, but a streak of 2 or 3 costs
  1 ms to 1.5 ms and leaves room for a mistake in the new accounting to show up
  as a delayed abort instead of a wrong one. `K` is cheap insurance, and the
  model says it is not load-bearing.
- Dropping the streak entirely would also delete the comment at
  `concurrency.c:152-166`, which is the record of WHY the number is 100. That
  record should stay, rewritten to say the threshold is no longer a bet on
  scheduler latency.

Whatever `K` becomes, the comment must stop claiming that the streak absorbs
"the transient over-count". After this fix there is no transient over-count to
absorb; the comment at `concurrency.c:154-156` and the one at
`concurrency.c:285-288` both say there is, and both become wrong.

#### The two dead fields

`main_in_wait` (`include/runtime.h:671`) is written once at
`concurrency.c:149`, initialised once at `deadlock.c:17`, and read NOWHERE.
`grep -rn main_in_wait src/ include/` finds exactly four lines: those three
plus a mention of the name inside a comment at `concurrency.c:170`. No line
reads the field. It steers nothing. The model keeps it only because the design
named it.

`current_goroutine` is the field section 8a item 1 of
`docs/superpowers/specs/2026-06-28-m8c-mt-scheduler-design.md` already tracks.
The M8c comment block at `concurrency.c:28-32` records that M8c leaves it
unused, and `goo_current_goroutine()` (`concurrency.c:277`) returns `t_current`
instead. The words "`->current_goroutine` fields (which are left unused" are on
`concurrency.c:30`, inside that block.
Section 8a item 1 offers three ways out: restore the field, sync it, or delete
the dead bookkeeping and document the deletion.

This fix needs `goo_current_goroutine()` and does not need
`g_scheduler->current_goroutine`, so the third option is the one it points at.
Deleting `main_in_wait` at the same time costs nothing and removes a field a
reader would otherwise assume the detector uses. Neither deletion is required
for the fix to be correct — both are cleanups the fix makes safe.
