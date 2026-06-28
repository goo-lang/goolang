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

Mirrors Go's `checkdead`: the deadlock is observed at the instant the last runnable participant commits to blocking, while it still holds the relevant locks (so we abort instead of `cond_wait`-ing into the hang).

### 3.1 Participants
- A **goroutine** is a participant from `goo_go` until exit.
- **`main`** (the OS thread running the user's `main`) is a participant until it enters `goo_scheduler_wait` — after which its body is done and it can no longer send/recv to rescue anyone.

### 3.2 State (added to `goo_deadlock_detector_t`, `include/runtime.h:213`)
- `int blocked_goroutines` — goroutines currently parked in a channel `cond_wait`.
- `int main_in_wait` — set when `main` enters `goo_scheduler_wait`.

(`stats.num_goroutines` — live goroutines — already exists. `g_scheduler->ready_queue` — runnable goroutines — already exists.)

### 3.3 Predicates (evaluated under the locks, at each block point)
There are **two evaluation sites** for the same predicate (`blocked_goroutines == num_goroutines` AND `ready_queue == NULL`, with `num_goroutines > 0` for the goroutine case):

- **At a channel block point (`goo_sched_block_begin`):**
  - **goroutine path** (`goo_current_goroutine() != NULL`): after `blocked_goroutines++`, deadlock iff the predicate holds AND `main_in_wait`. (Prompt detection when `main` is already waiting.)
  - **main path** (`goo_current_goroutine() == NULL`): deadlock iff the predicate holds (no `main_in_wait` guard — `main` itself blocking *is* the last-participant event).
- **In `goo_scheduler_wait`'s poll loop:** each iteration, deadlock iff `blocked_goroutines == num_goroutines` AND `num_goroutines > 0` AND `ready_queue == NULL`. This loop runs only after `main`'s body is done (so `main` can no longer act), and it **closes the timing race**: if the last goroutine blocked *before* `main` reached `goo_scheduler_wait`, `block_begin` saw `main_in_wait==false` and did not abort — the poll loop catches it (within one 0.5ms tick).

Rationale for the `main_in_wait` guard on the goroutine block path: until `main` reaches `goo_scheduler_wait`, it may still send/recv and rescue a blocked goroutine, so a goroutine blocking while `main`'s body runs is **never** declared a deadlock at `block_begin`. This (together with the poll-loop check only running post-`main`-body) is what prevents false positives in the common `go producer(c); x := <-c` shape.

### 3.4 On detection
Write exactly `fatal error: all goroutines are asleep - deadlock!\n` to **stderr** and terminate with **exit code 2** (Go's behavior). No core dump, no cycle dump.

### 3.5 Locking
Block points already hold `ch->mutex`. The helpers take `scheduler_mutex` internally → lock order is **`ch->mutex` then `scheduler_mutex`**, which is safe: no existing path takes `scheduler_mutex` then a channel mutex. The increment + predicate evaluation happen as one critical section under `scheduler_mutex`, so the all-asleep snapshot is consistent and false-positive-free (a participant counted as blocked is committed — it holds its `ch->mutex` and will `cond_wait` next).

---

## 4. Components / touchpoints

| Unit | File | Change |
|---|---|---|
| accounting helpers + state | `src/runtime/concurrency.c` | `void goo_sched_block_begin(void)` (goroutine-vs-main via `goo_current_goroutine()`; `scheduler_mutex`; update `blocked_goroutines`; evaluate the §3.3 block-point predicate; on deadlock call the abort); `void goo_sched_block_end(void)` (decrement for the goroutine path). In `goo_scheduler_wait` (`:119`): set `main_in_wait` at the top, AND in the poll loop evaluate the §3.3 poll-loop predicate and abort on deadlock (closes the timing race). Remove the vestigial `goo_deadlock_check` call from the idle branch (`:303`). |
| block-point bracketing | `src/runtime/channels.c` | bracket each of the 5 `pthread_cond_wait` block points (`:254`, `:291`, `:309`, `:372`, `:407`) with `goo_sched_block_begin()` before the wait and `goo_sched_block_end()` after. |
| state fields | `include/runtime.h:213` | add `blocked_goroutines`, `main_in_wait` to `goo_deadlock_detector_t` (header change ⇒ `make clean`). |
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
- **Goroutine-path case:** a second `.goo` where a spawned goroutine receives on an empty channel and `main` never sends (falls through to `goo_scheduler_wait`) — exercises the goroutine predicate + `main_in_wait` guard. Same assertion.
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
