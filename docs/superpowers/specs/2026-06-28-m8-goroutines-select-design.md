# M8 — Goroutines, Unbuffered Channels & Select: Concurrency Substrate

**Status:** Shipped (2026-06-28)
**Milestone:** M8
**Predecessor:** M7 (basic buffered channels) merged @ `2489c68`.
**Theme:** Concurrency / dataflow substrate, phase 2 — make the CSP core
(`go` spawning, unbuffered rendezvous channels, blocking `select`) actually
compile and run. This is the foundation the other concurrency paradigms are
built on.

---

## 1. Design reassessment — why CSP is the substrate

The request was for goroutines plus Rust-, ZeroMQ-, Erlang- and OpenMP-style
parallelism + concurrency. Rather than build four independent subsystems, we
reassessed and chose the **minimal substrate from which the rest can be built in
Goolang itself**: **goroutines + buffered/unbuffered channels + blocking
`select`** (CSP). The four target paradigms layer on top:

| Paradigm | How it layers on the CSP substrate | Home |
|---|---|---|
| **Erlang actors** | actor = goroutine running `for { msg := <-mailbox; … }`; mailbox = buffered channel; address = channel handle. Supervision needs one extra runtime hook (goroutine-exit/join signal) **or** working `defer`/`recover`. | Goolang stdlib (+ tiny runtime hook) |
| **ZeroMQ patterns** (pub/sub, req/rep, push/pull) | routing over basic channels; the runtime already routes these (`goo_chan_subscribe`/`pair_req_rep`/`add_worker`). Needs codegen wiring for the pattern channel types. | runtime routing + Goolang ergonomic layer → **M9** |
| **OpenMP parallel-for** | `parallel_for(n, body)` = goroutine fan-out over chunks + `WaitGroup` (`goo_waitgroup_*` already correct). Needs real OS threads for speedup — the reason the pthread scheduler is kept. | Goolang stdlib (+ WaitGroup binding) |
| **Rust safety** | send = move: a compile-time ownership pass over `ch <- v` / `<-ch` (`src/types/ownership_checker.c`, `escape_analysis.c`). No runtime primitive. | type/ownership checker |

**Cannot be built purely in Goolang on the substrate** (need a runtime
primitive): unbuffered rendezvous (shipped in M8d), real parallelism (OS threads,
present), supervision completion/panic notification (deferred), and `select` with
`time.After` timeout (the timeout plumbing exists in `goo_select`; a timer source
is deferred).

---

## 2. What M8 shipped

All probe-gated, verified locally (binary diff + `opt --passes=verify`), wired
into `make verify` and `.github/workflows/tests.yml`.

- **8a — scheduler lifecycle (`go-probe`).** `goo_scheduler_wait()`
  run-to-completion barrier (`src/runtime/concurrency.c`) emitted before
  generated `main` returns (`src/codegen/function_codegen.c`), so goroutine side
  effects are observable before exit. The scheduler is lazily created by the
  first `goo_go()`; the barrier is a no-op when no goroutine ran, so
  non-concurrent programs are unaffected.
- **8b/8c — `go f(args)` (`go-probe`).** `codegen_generate_go_stmt` heap-boxes
  the evaluated arguments and generates a per-call-site thunk
  (`void __goo_thunk_N(i8*)`) that unboxes, calls the real function, and frees
  the box — adapting any user signature to the runtime's `void(*)(void*)`.
  All LLVM types built in `codegen->context` (M7-class fix). Closures/methods as
  `go` targets are rejected with a clear diagnostic (deferred).
- **8d — unbuffered rendezvous (`unbuffered-probe`).** `make_chan(T)` /
  `make_chan(T, 0)` produce a rendezvous channel; `goo_chan_send`/`recv`
  implement a 1-slot handoff (`rv_slot`/`rv_full`) where send returns only after
  a receiver takes the value. Type checker now accepts the unbuffered forms.
- **8e — blocking `select` (`select-probe`).** `goo_select` polls the active
  cases and blocks per the timeout policy (`0` = non-blocking/default present,
  `-1` = block, `>0` = deadline). The select codegen was fixed end-to-end:
  context fixes, a corrected per-slot element GEP, `is_send` stored as i32, recv
  scratch sized from the channel element type, and NULL-channel default slots so
  the runtime index aligns with the case blocks.

### Scheduler-model decision
Kept the existing **pthread + ucontext** runtime (so OpenMP-style speedup stays
reachable) but initialize with **`num_threads = 1`** for now: the multi-thread
path shares one `main_context`/`current_goroutine` and is a latent race.
Single-thread + the cross-thread (goroutine ↔ main) rendezvous in the probes is
race-free. Probes assert order-independent output (sums/counts) because the
scheduler interleaves nondeterministically.

---

## 3. Deferred work

- **M9:** pattern-channel codegen (pub/sub, req/rep, push/pull) over the existing
  routing runtime; condvar-based (non-polling) `select` wakeup; `select` timeout
  + `time.After`.
- **M10:** Erlang supervision (needs `defer`/`recover` — `defer` is still a stub
  at `statement_codegen.c` — or a goroutine join-channel hook); OpenMP
  `parallel_for` stdlib + `WaitGroup` Goolang binding; ownership-transfer-on-send
  enforcement; multi-threaded scheduler (per-thread context) for real parallelism.
- Go-compat follow-ups: `make(chan T, n)` syntax; comma-ok receive
  `v, ok := <-ch`; `go` on closures/methods.
