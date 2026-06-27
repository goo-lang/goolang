# M8 — Goroutine Spawning (`go f(ch)`): Verify-and-Fix

**Status:** Design (approved 2026-06-27)
**Milestone:** M8
**Predecessor:** M7 (basic channels, in-process buffered) shipped @ `main` `2489c68` (PR #27).
**Theme:** Concurrency, phase 2 — make `go` actually spawn a goroutine that runs and
communicates back over a buffered channel. First real concurrency in Goo. Unbuffered
rendezvous (M9) and `select` (M10) follow.

---

## 1. Summary

Goo has a real threading runtime — `goo_scheduler_init` + `pthread_create` +
`goo_go(func, void* arg)` (`src/runtime/concurrency.c`) — and the grammar parses
`go call_expr`. But `go` does not work: `codegen_generate_go_stmt`
(`src/codegen/statement_codegen.c:691`) builds its LLVM types in the **global context**
(the same defect M7 fixed for channels → `verifyModule` failure) **and** hardcodes
`NULL` as the goroutine argument ("for simplicity"), so a spawned goroutine cannot
receive the channel it needs to communicate. M8 makes `go f(ch)` spawn a goroutine with
its single pointer-sized argument, probe-gated and deterministic. *Verify-and-fix*;
codegen + a typecheck rejection; no grammar/runtime changes.

---

## 2. Locked scope decisions

1. **Single pointer-sized argument.** `go f(ch)` passes the call's one argument — a
   channel, which lowers to `i8*` — as `goo_go`'s `void* arg`. A goroutine function
   taking one pointer-sized parameter (`func produce(ch chan int)`) lowers to
   `void(i8*)`, exactly `goo_goroutine_func_t` — so the channel is passed **directly, no
   marshaling struct**.
   Rejected: full multi-arg marshaling (`go worker(ch, done, i)`) — heap-pack all args
   into a struct with lifetime management; far more codegen; a clean follow-up increment.
   Rejected: niladic + package-level channel (`go f()` over a global) — leans on
   package-level var reads (separately unverified) and is less Go-idiomatic.

2. **Reject out-of-scope `go` forms at compile time** (mirrors M7's capacity-0
   rejection), so they error loudly instead of silently dropping arguments:
   - `go f(a, b, …)` (more than one argument)
   - `go f(x)` where `x` is not pointer-sized (e.g. an `int` value)
   `go f()` (niladic, arg = NULL) is allowed (harmless; useful only via globals).

3. **Codegen-only fix + one typecheck rejection.** The runtime and grammar are correct.
   Rejected: runtime changes (the scheduler/`goo_go` already work).

4. **No new grammar.**

---

## 3. Architecture / touchpoints

| Stage | File | Work |
|---|---|---|
| Codegen — go stmt | `src/codegen/statement_codegen.c` `codegen_generate_go_stmt` (~691) | build all LLVM types in `codegen->context` (fix the M7-class context bug); pass the call's single argument as `goo_go`'s `void* arg` (bitcast to `i8*`) instead of `NULL`; declare `goo_go` with an in-context signature |
| Type check — go stmt | `src/types/*` (the `AST_GO_STMT` / call path) | reject multi-arg and non-pointer-arg `go` calls with a clear M8-scope diagnostic |
| Runtime | `src/runtime/concurrency.c` | **no change** — `goo_go`, `goo_scheduler_init`, pthread scheduler already implemented |
| Grammar | `src/parser/parser.y` | **no change** — `go call_expr` already parses (incl. args) |

No new files.

---

## 4. Calling convention

- `goo_go(goo_goroutine_func_t func, void* arg)` where `goo_goroutine_func_t = void(*)(void*)`.
- A Goo channel lowers to `i8*` (opaque pointer to the runtime channel). `void*` is `i8*`.
- Therefore `func produce(ch chan int)` → LLVM `void(i8*)` ≡ `goo_goroutine_func_t`, and
  `go produce(c)` lowers to `goo_go(produce, (i8*)c)` — the channel handle passed straight
  through as the goroutine argument. No argument-packing struct is needed for the
  single-channel case.
- The goroutine body then sends on `ch`; `main` receives on the same buffered channel —
  the receive blocks `main` until the goroutine delivers, giving a deterministic result.

---

## 5. Surface & semantics (exact, locked)

| Construct | Semantics |
|---|---|
| `go f(ch)` | spawn a goroutine running `f` with the single pointer-sized arg `ch` |
| `go f()` | spawn niladic `f` (arg = NULL) — allowed; only useful via globals |
| `go f(a, b, …)` | **compile error** — multi-arg goroutines not supported yet |
| `go f(x)` where `x` is not pointer-sized | **compile error** — only a single pointer-sized arg is supported yet |

**Out of scope (later milestones):** multi-arg goroutines (marshaling), unbuffered
rendezvous (M9), `select` (M10), `defer` (stub today, separate), goroutine panic/exit
semantics.

---

## 6. Probe design — `go-probe`

Deterministic by construction (a buffered channel is the join barrier — `main`'s receive
blocks until the goroutine sends):

```go
package main
import "fmt"

func produce(ch chan int) {
    ch <- 7
}

func produce9(ch chan int) {
    ch <- 9
}

func main() {
    c := make_chan(int, 1)
    go produce(c)
    x := <-c          // blocks until the goroutine delivers
    fmt.Println(x)    // 7

    d := make_chan(int, 1)
    go produce9(d)
    fmt.Println(<-d)  // 9 — second goroutine, second channel
}
```

Exact-value assertions (`7` then `9`). Must FAIL before the fix
(verifier error from the global-context bug, or a dropped/garbage argument), PASS after,
`opt --passes=verify` clean. **Run under a timeout** (e.g. `timeout 10 ./build/go_probe`)
so a concurrency hang fails loudly instead of blocking the gate.

---

## 7. Scheduler lifecycle (spike deliverable)

`goo_go` auto-initializes the scheduler (`goo_scheduler_init(1)` when `g_scheduler` is
NULL). The buffered receive makes the *result* deterministic, but Task 1 begins with a
spike that confirms the program **exits cleanly (rc 0)** with a live scheduler thread —
no hang at process teardown and no required explicit `goo_scheduler_shutdown` in user
code. If a teardown hang appears, the minimal fix (e.g. detached worker threads, or a
shutdown hook on `main` return) is in scope; anything larger is flagged and deferred. The
spike records the finding in the PR/ledger.

---

## 8. Testing / CI gates

- `go-probe` added to BOTH `verify:` (Makefile) and `.github/workflows/tests.yml:54`,
  invoked **under a timeout** so a hang is a loud failure.
- Full pre-existing probe gate (30 CI probes after M6+M7) stays green — special attention
  to the channel probes (`chan-probe`, `chan-elem-probe`, etc.) since `go` builds on them.
- `opt --passes=verify` clean on `go-probe`.
- `make test` (unit suite) shows no new failures.
- Verified LOCALLY, since CI on `dd0wney` may be billing-blocked (jobs show red but never
  start).

---

## 9. Known limitations / deferred work

- **Multi-arg goroutines** (`go worker(ch, done, i)`) — rejected now; a marshaling-struct
  increment later.
- **M9:** unbuffered rendezvous (capacity-0 direct handoff; currently rejected at
  typecheck by M7).
- **M10:** `select`.
- `defer` is a stub (runs the call inline today) — separate milestone.
- Networked/distributed goroutines and channel endpoints remain unimplemented (separate
  large milestone; confirmed inert in M7).
- The concurrency demos (`actor_system_demo.goo`, etc.) stay aspirational (they use
  closures `go func(){…}`, multi-arg spawns, and select — all deferred).

---

## 10. Success criteria

M8 is complete when:
1. `go-probe` compiles, runs (under timeout), and diff-matches its expected output — a
   spawned goroutine delivers a value that `main` receives over a buffered channel.
2. `opt --passes=verify` is clean on the probe's IR.
3. Out-of-scope `go` forms (multi-arg, non-pointer arg) are rejected at compile time.
4. It is wired into `tests.yml` and `make verify`.
5. The full pre-existing probe suite (30 CI probes) remains green (no regressions).
6. Clean program exit with a live scheduler is confirmed (spike deliverable, §7).
7. Verified locally (CI may be billing-blocked).
