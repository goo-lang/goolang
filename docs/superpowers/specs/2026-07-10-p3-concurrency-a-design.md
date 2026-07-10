# Phase 3 sub-project A — channel lifecycle & scheduler exit (P3.1, P3.2, P3.3, P3.5)

Date: 2026-07-10. Branch: `feat/p3-concurrency-a`. Roadmap: docs/2026-07-08-v1-roadmap.md Phase 3.

## User decisions (2026-07-10, all confirmed)

- **P3.3 main-exit = Go parity**: process exits when main returns; goroutines abandoned, exactly
  like Go. Reverses the deliberate M8 wait-all superset. Justification: the join was emitted
  inconsistently (fall-off-the-end main only — explicit `return` bypassed it), a busy-loop
  goroutine hung the program forever, and no golden fixture depends on wait-all (all goroutine
  probes sync via channel recv in main).
- **P3.5 recover() = minimum v1 diagnostic**: clean compile-time rejection; full unwinding post-v1.
- (P3.9 nil-map write = Go-parity panic — decided same day, lands in sub-project C, not here.)

## Recon facts (verified 2026-07-10)

- `goo_chan_close` exists and works: `src/runtime/channels.c:79-92` (sets `closed`, broadcasts
  both conds); declared `include/runtime.h:360`. Frontend never recognizes `close` — today:
  `Type error: Undefined variable 'close'`.
- Comma-ok recv is close-ready: `function_codegen.c:1978-2031` calls `goo_chan_recv` exactly once,
  feeds its i32 status into `ok`. Verified running on main (buffered chan → `7 / true`, exit 0).
- **Gap**: `goo_chan_recv` returns 0 on closed+drained (`channels.c:391-393` buffered,
  `:414-419` rendezvous) **without writing the out-buffer** — `v` would be uninitialized garbage.
- Wait-all barrier: `goo_scheduler_wait` (`concurrency.c:143-194`), emitted only at
  `function_codegen.c:1217-1229` when `is_entry_main` falls off the end. Deadlock detector's
  in-main instant path (`goo_sched_block_begin`, `concurrency.c:289-317`) is independent of it.
- C stress tests call `goo_scheduler_wait` directly (`tests/concurrency/*_stress.c`) — the
  runtime symbol must stay; only the codegen emission changes.
- select `v, ok :=` binding intentionally rejected at `type_checker.c:3415-3422` pending close().

## Tasks

### A1 (P3.1) — `close(ch)` builtin

- **Typecheck**: add `"close"` beside make/append/len/cap (`expression_checker.c` ~3074-3299):
  exactly one arg, arg must be TYPE_CHANNEL (positioned error naming the actual type otherwise),
  result void (statement context).
- **Codegen**: lower in `call_codegen.c` beside the `len` builtin (~:593): declare-if-needed and
  call `void goo_chan_close(ptr)`.
- **Runtime zero-value contract**: on the closed+drained failure returns in `goo_chan_recv`
  (both buffered and rendezvous paths) and `goo_chan_try_recv`, `memset(data, 0, elem_size)`
  before `return 0`. One fix covers comma-ok, single-value recv, and P3.2's range loop.
- **Go-parity panics** (all currently-unreachable, zero regression risk): `goo_chan_close` on
  already-closed → `goo_panic("close of closed channel")`; on NULL ch →
  `goo_panic("close of nil channel")`; blocking send and try-send on closed →
  `goo_panic("send on closed channel")` (replaces today's silent `return 0`).
- **Probes**: golden run fixture `close_chan` (producer closes; consumer sees value/ok=true then
  zero/ok=false; exit 0); reject fixtures `close_nonchan` (close on int) and `close_arity`
  (zero and two args). Expected-exit fixture for double-close panic (exit 2).

### A2 (P3.2) — `for v := range ch`

- Lower to a loop: `goo_chan_recv` once per iteration; status 0 (closed+drained) → break.
  Bare `for range ch` (no value var) also supported (parse rule already exists for strings).
- Typecheck: range over TYPE_CHANNEL yields one value of the element type (no index var; Go
  rejects the two-var form on channels — we reject it too, positioned error).
- **Probe**: go-produce/close/range-consume prints all values, exits 0, no deadlock message.

### A3 (P3.3) — Go-parity main exit

- Remove the `goo_scheduler_wait` emission at `function_codegen.c:1217-1229` (delete the call,
  keep defers/return). Keep the runtime symbol and the deadlock detector wiring untouched.
- Spec note in docs/02-LANGUAGE-SPECIFICATION.md: main-return abandons goroutines (Go parity);
  supersedes the M8 wait-all note.
- **Probes**: busy-loop-goroutine program exits 0 promptly (timeout-guarded golden fixture);
  existing channel/goroutine probes stay green (they all sync in main).

### A4 (P3.5) — recover() diagnostic

- Recognize `recover` in the expression checker's call path and reject with
  `recover() is not supported in v1; panics terminate the program (use !T error unions for
  recoverable errors)` — positioned, no 'Undefined variable' cascade (bind TYPE_POISON per the
  P2.8 convention).
- **Probe**: reject fixture `recover_v1`.

### Rider R1 — select `v, ok :=` unlock

After A1+A2, assess lifting the rejection at `type_checker.c:3415-3422` by wiring the select
recv-case binding to the same status-fed comma-ok pattern. If not cheap, reword the diagnostic
to point at the workaround (recv then check ok outside select) and defer.

## Acceptance / gates

- Per-commit: `make lexer && make test` + new probes green; tripwire untouched (no grammar
  changes expected except possibly A2's range form — if parser.y is touched, goo-grammar skill
  rules apply: tripwire 121/256 exact before and after).
- Pre-PR: `make verify-core` all green; golden/reject counts strictly increased, zero failures.
- Exit shape: producer/consumer program (goroutines, close, range-over-channel) compiles, runs
  correctly, terminates — the Phase 3 exit-gate core.
