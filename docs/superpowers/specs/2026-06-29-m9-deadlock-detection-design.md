# M9 — Channel-Deadlock Detection (global "all asleep", block-time)

**Status:** Design (approved 2026-06-29)
**Milestone:** M9 (concurrency diagnostics)
**Predecessor:** M8d (PR #33 merged, `main` @ `fde1c66`) — the last §8a pre-default-flip prerequisite.
**Theme:** Turn a fully-deadlocked Goo program from a silent hang into Go's `fatal error: all goroutines are asleep - deadlock!` + non-zero exit. The final prerequisite before the multi-core default-flip.

---

## 1. Summary

Goo's channel blocking uses `pthread_cond_wait` (`src/runtime/channels.c`), so a deadlocked program hangs forever with no diagnostic. The existing deadlock detector is non-functional on three counts and architecturally mismatched (see §2). M9 replaces it with a Go-faithful **global "all goroutines asleep" check performed at block-time**: at the moment the last runnable participant is about to block on a channel, it detects that no participant can make progress and aborts with Go's message and exit code 2. This is **diagnostics, not correctness** — a deadlocked program already hangs; M9 makes the hang observable.

**Out of scope:** subset/partial wait-cycle detection (Go's runtime does not report these either); detecting deadlocks that are not "all participants asleep".

---

## 2. Why the existing detector is replaced, not repaired

`src/runtime/deadlock.c` is dead on three independent counts, and its model cannot work with `cond_wait` blocking:
1. **Never called in a real deadlock.** `goo_deadlock_check` runs only from the scheduler loop's idle branch (`concurrency.c:303`). But a channel-blocked goroutine parks its *worker OS thread* in `pthread_cond_wait`; when all are blocked, no worker reaches the idle branch — nobody runs the detector.
2. **Wrong population.** `detect_deadlock` scans `g_scheduler->ready_queue` for `GOO_GOROUTINE_BLOCKED` goroutines, but blocked goroutines are `cond_wait`-parked, never in `ready_queue`.
3. **Empty edges.** Its cycle DFS reads `channel->send_waiters`/`recv_waiters`, which are **never populated** anywhere in the runtime.

Plus `DEBUG`/`FATAL`/`WARNING` `printf`s throughout. M9 removes `detect_deadlock`, `detect_cycle_dfs`, `goroutine_depends_on`, and these printfs rather than repairing a model that doesn't fit.

---

## 3. Detection model (block-time global all-asleep)

> ### ⚠ THE PROTOCOL IN §3.2–§3.3 IS SUPERSEDED (2026-09-02, commit `3d411d91`)
>
> Read this box first. The rest of §3 is the 2026-06-29 design as approved, kept
> because §2 and §3.5 still hold and because the reasoning it records is what
> the correction had to overturn. Where the two disagree, the box wins.
>
> **What was wrong.** The design counted a goroutine as blocked from
> `goo_sched_block_begin` until it returned from `pthread_cond_wait` and ran
> `goo_sched_block_end`. That interval is scheduler latency, which no OS
> bounds, so `blocked_goroutines == num_goroutines` was transiently TRUE for
> LIVE programs. §3.3's answer was a stability streak — 3 polls, later raised
> to 100 when 3 proved too few under 4 workers. A SPIN model of the protocol
> proved that answer cannot work: **for every finite streak there is a live
> program the detector aborts.** The streak buys time, never correctness.
>
> **What replaces it.** `blocked_goroutines` is the size of a SET, and a
> goroutine leaves that set at the instant its waker decides to wake it, not
> when it is next scheduled:
>
> - `goo_sched_block_begin(goo_goroutine_t** waiters)` is a set INSERTION. It
>   sets the goroutine's `counted_blocked` bit, pushes it on the waiter list of
>   the condition variable it is about to wait on, and increments the count —
>   under `scheduler_mutex`, with the channel mutex held.
> - `goo_sched_wake_waiters(goo_goroutine_t** waiters)` is new. Every wake site
>   calls it immediately BEFORE its broadcast, and it removes EVERY member of
>   that condvar's waiter list from the set. The set the waker empties is
>   exactly the set the broadcast wakes.
> - `goo_sched_block_end(goo_goroutine_t** waiters)` is an IDEMPOTENT
>   unregister, not a decrement. On the woken path the waker already removed
>   this goroutine, so it is a no-op; it exists to stop a SPURIOUS return from
>   `pthread_cond_wait` leaking a set member.
> - Every wake is a `pthread_cond_broadcast`. `pthread_cond_signal` gives the
>   caller no way to learn which waiter the OS woke, so a signal site cannot
>   pair itself with "clear the bit of the one waiter this wakes". The cost is
>   a thundering herd on a condvar with many waiters, and it is not yet
>   measured at scale.
> - The poll loop's decision and `goo_deadlock_abort()` are now in ONE critical
>   section. The old loop dropped `scheduler_mutex` between reading the three
>   conjuncts and aborting, so the state could move in between.
> - The streak is **3**, and it is no longer load-bearing. The model proves the
>   protocol correct at a streak of 1; 3 costs 1.5 ms on a program that is
>   already dead and makes a mistake in the new accounting show up as a late
>   abort instead of a wrong one. The 100 is gone.
> - `main_in_wait` is DELETED. It had one write, one initialisation and no
>   read, so §3.1's "main is a participant until it enters `goo_scheduler_wait`"
>   was never enforced by that field. The poll loop runs only inside
>   `goo_scheduler_wait`, which is where the fact lives now.
>
> **Where the proof and the gates are.**
>
> - The SPIN model, both results, and the three rulings the C code implements:
>   `tests/models/deadlock_detector.pml` and `tests/models/README.md`. Gated by
>   `spin-deadlock-model-probe` in `make verify-core`.
> - `false-deadlock-probe` (`scripts/false_deadlock_probe.sh`,
>   `tests/concurrency/false_deadlock_test.c`): a LIVE program must survive a
>   100 ms wake delay. RED against the old protocol, GREEN against this one.
> - `deadlock-poll-probe` (`scripts/deadlock_poll_probe.sh`,
>   `tests/concurrency/deadlock_poll_test.c`): two goroutines park for ever and
>   the process must leave through the poll loop with exit 2. `deadlock-probe`
>   does NOT reach that loop — it aborts in `goo_sched_block_begin`'s
>   main-thread branch, §3.3's second bullet.
> - Both have a `--self-test` that mutates one runtime translation unit and
>   requires the gate to go red.

Mirrors Go's `checkdead`: the deadlock is observed at the instant the last runnable participant commits to blocking, while it still holds the relevant locks (so we abort instead of `cond_wait`-ing into the hang).

### 3.1 Participants
- A **goroutine** is a participant from `goo_go` until exit.
- **`main`** (the OS thread running the user's `main`) is a participant until it enters `goo_scheduler_wait` — after which its body is done and it can no longer send/recv to rescue anyone.

### 3.2 State (added to `goo_deadlock_detector_t`, `include/runtime.h:213`)
- `int blocked_goroutines` — goroutines currently parked in a channel `cond_wait`.
  **Since 2026-09-02 this is the size of a set, not a count of parked threads:**
  a goroutine leaves it at the wake site. See the box above.
- ~~`int main_in_wait` — set when `main` enters `goo_scheduler_wait`.~~
  **REMOVED 2026-09-02** (`3d411d91`): one write, one initialisation, no read.
- Added 2026-09-02, in `struct goo_goroutine`: `int counted_blocked` (the set
  membership bit) and `struct goo_goroutine* next_waiter` (the link in one
  condvar's waiter list). `goo_channel_t`'s `send_waiters`/`recv_waiters`,
  declared and empty since M8, are those lists.

(`stats.num_goroutines` — live goroutines — already exists. `g_scheduler->ready_queue` — runnable goroutines — already exists.)

### 3.3 Predicates (evaluated under the locks, at each block point)
> **Note (revised during implementation for false-positive safety).** The original design also evaluated the predicate for the *goroutine path* inside `goo_sched_block_begin`. That proved unsafe: `blocked_goroutines` transiently over-counts a goroutine that has been `pthread_cond_signal`'d but has not yet returned from `cond_wait` to call `block_end`, so an instantaneous goroutine-path check can false-positive (spuriously abort a working program). Since **no false positives** is the primary criterion, the goroutine-path check was removed; detection for goroutines is deferred to the poll loop with a **stability requirement**. The two sites are now:

- **At a channel block point (`goo_sched_block_begin`):**
  - **goroutine path** (`goo_current_goroutine() != NULL`): increment `blocked_goroutines` and return. **No deadlock check** (deferred to the poll loop — avoids the over-count race).
  - **main path** (`goo_current_goroutine() == NULL`): deadlock iff `num_goroutines == 0` AND `ready_queue == NULL` (no goroutine exists that could ever signal `main`); and if `!g_scheduler` (no goroutine was ever spawned), `main` blocking on a channel is an immediate deadlock.
- **In `goo_scheduler_wait`'s poll loop (authoritative for goroutine deadlocks):** each ~0.5ms iteration, evaluate `all_asleep = (num_goroutines > 0 && blocked_goroutines == num_goroutines && ready_queue == NULL)`; track a consecutive streak; abort only once `all_asleep` holds for **3 consecutive polls** (~1.5ms). A transient over-count resolves in microseconds and cannot persist across 3 polls → false-positive-free; a genuine deadlock holds forever → detected. The loop runs only after `main`'s body is done, so `main` can no longer rescue anyone.

~~`main_in_wait` is still set at the top of `goo_scheduler_wait`~~ — the field is gone (2026-09-02). False positives in the common `go producer(c); x := <-c` shape are prevented because the goroutine path never aborts, the poll loop only runs post-`main`-body, and — since `3d411d91` — a woken goroutine is out of the blocked set before its waker broadcasts, so the equality cannot hold while any goroutine is live. The stability streak is no longer what prevents them; see the box at the head of §3.

**Known gap (accepted, documented):** a deadlock where `main` blocks on a channel *mid-body* while one or more goroutines are *also* all blocked (`num_goroutines > 0`) is **not** detected — `main` is not in the poll loop, and an instantaneous check at its block point cannot safely decide without risking a false positive. Go catches this because `main` is itself a goroutine; Goo's `main` is an OS thread. Closing it would need a `main`-side quiescence mechanism (future work).

### 3.4 On detection
Write exactly `fatal error: all goroutines are asleep - deadlock!\n` to **stderr** and terminate with **exit code 2** (Go's behavior). No core dump, no cycle dump.

### 3.5 Locking
Block points already hold `ch->mutex`. The helpers take `scheduler_mutex` internally → lock order is **`ch->mutex` then `scheduler_mutex`**, which is safe: no existing path takes `scheduler_mutex` then a channel mutex. The increment + predicate evaluation happen as one critical section under `scheduler_mutex`, so the all-asleep snapshot is consistent and false-positive-free (a participant counted as blocked is committed — it holds its `ch->mutex` and will `cond_wait` next).

---

## 4. Components / touchpoints

| Unit | File | Change |
|---|---|---|
| accounting helpers + state | `src/runtime/concurrency.c` | AS SHIPPED SINCE 2026-09-02: `void goo_sched_block_begin(goo_goroutine_t** waiters)` (set insertion), `void goo_sched_wake_waiters(goo_goroutine_t** waiters)` (bulk unregister at the wake site, before the broadcast), `void goo_sched_block_end(goo_goroutine_t** waiters)` (idempotent unregister). In `goo_scheduler_wait`: the poll loop evaluates the all-asleep conjunction and aborts, both inside one `scheduler_mutex` critical section. No `main_in_wait`. The vestigial `goo_deadlock_check` call is gone from the idle branch. |
| block-point bracketing | `src/runtime/channels.c` | bracket each of the 5 `pthread_cond_wait` block points (`:254`, `:291`, `:309`, `:372`, `:407`) with `goo_sched_block_begin()` before the wait and `goo_sched_block_end()` after. |
| state fields | `include/runtime.h` | `blocked_goroutines` in `goo_deadlock_detector_t`; `counted_blocked` and `next_waiter` in `struct goo_goroutine` (both added 2026-09-02). `main_in_wait` was added by M9 and removed again by `3d411d91`. Header change ⇒ `make clean`. |
| strip dead detector | `src/runtime/deadlock.c` | remove `detect_deadlock`/`detect_cycle_dfs`/`goroutine_depends_on` + all `DEBUG`/`FATAL`/`WARNING` printfs; provide the minimal `goo_deadlock_abort()` (message + `exit(2)`) the helpers call; keep/trim the enable/flag API as needed. |
| declarations | `include/runtime.h` | declare `goo_sched_block_begin`/`goo_sched_block_end` if cross-TU (channels.c calls them). |

No grammar/codegen change. Default thread count unchanged (still 1).

---

## 5. Testing / CI gates

- **`deadlock-probe` (RED→detection):** a `.goo` that deadlocks — `main` receives on an empty buffered channel with no sender:
  ```go
  package main
  func main() {
      c := make_chan(int, 1)
      x := <-c   // no sender — all participants asleep
      _ = x
  }
  ```
  Custom Make target: run under `timeout`. **Before M9 it hangs (exit 124); after, it must exit 2 within the timeout with stderr containing `all goroutines are asleep - deadlock!`** (assert: not 124, exit==2, stderr matches).
- **Goroutine-path case:** a spawned goroutine receives on an empty channel and `main` never sends (falls through to `goo_scheduler_wait`) — exercises the poll loop. SHIPPED AS `deadlock-poll-probe` (2026-09-03, two parked goroutines, `tests/concurrency/deadlock_poll_test.c`), which is the only gate that reaches the poll loop; `deadlock-goroutine-probe` covers the Go-parity case where main EXITS and abandons a blocked goroutine (exit 0).
- **⚠️ No-false-positive gate (primary safety criterion):** the entire existing channel/go/escape/stress suite + `make test` must stay green. A correct program (incl. `go producer(c); x := <-c`, `chan-mt-stress`, `unbuffered-probe`, `select-probe`) must **never** trip the detector. A spurious abort is the worst possible outcome, so this gate carries the most weight.
- New targets wired into `verify:` + `.github/workflows/tests.yml`, all under `timeout`.
- Header change ⇒ `make clean && make goo`.
- Verified **locally** (CI on `dd0wney` may be billing-blocked).

---

## 6. Out of scope (later)

- Flipping the default thread count to multi-core (the payoff M9 unblocks — a separate change).
- Subset/partial wait-cycle detection (Go does not report it).
- `select`-statement deadlock accounting beyond the channel block points (the current `select` polls; if a future condvar-based `select` adds block points, they get the same bracketing).
- Reporting *which* goroutines/channels are involved (Go prints a goroutine dump; M9 prints only the headline message).

---

## 7. Success criteria

M9 is complete when:
1. `deadlock-probe` exits 2 (not 124/hang) with stderr `fatal error: all goroutines are asleep - deadlock!` — before M9 it hung.
2. The goroutine-path deadlock case is detected too (goroutine blocks, `main` in `goo_scheduler_wait`).
3. No false positives: the full pre-existing channel/go/escape/stress suite + `make test` (76/1) stay green.
4. The dead `detect_deadlock`/`detect_cycle_dfs`/`goroutine_depends_on` code and `DEBUG`/`FATAL` printfs are removed.
5. Default thread count unchanged; new probes wired into `verify:` + `tests.yml`.
6. Verified locally (CI may be billing-blocked).
