# M7 — Basic Channels End-to-End (in-process, buffered): Verify-and-Fix

**Status:** Design (approved 2026-06-27)
**Milestone:** M7
**Predecessor:** M5 (`!T` error unions) shipped @ `main` `84947a7`; M6 (block-scope teardown) is PR #26, open — M7 is independent of it.
**Theme:** Concurrency / dataflow substrate, phase 1 — make the basic channel value path
(`make_chan` / `<-` send / `<-` recv) actually compile to valid IR and run in-process
over a buffered channel. This is the prerequisite for all later concurrency and for the
pattern-channel (ZeroMQ-style) dataflow work.

---

## 1. Summary

Goo has a substantial channel implementation — grammar (`chan T`, pattern channels),
type checker (`type_check_make_chan_call`), codegen (`make_chan`/send/recv), and a **real
runtime** (`src/runtime/channels.c`: ring-buffer channels plus pub/sub/req-rep/push-pull
pattern support). But **none of it runs**: a minimal `ch := make_chan(int,1); ch <- 42;
x := <-ch` fails LLVM `verifyModule`. M7 fixes the basic value path end-to-end,
probe-gated. *Verify-and-fix*; codegen-only; no grammar, runtime, or type-system changes.

### Measured failure (the starting point)
```
Module verification failed: Call parameter type does not match function signature!
  %send_result = call i32 @goo_chan_send(ptr %ch1, ptr %value_as_void_ptr)
Function context does not match Module context!  ptr @goo_make_chan
```

### Root cause
The channel codegen builds its LLVM types and constants with **global-context** builders
(`LLVMInt8Type()`, `LLVMInt32Type()`, `LLVMInt64Type()`,
`LLVMPointerType(LLVMInt8Type(),0)`, `LLVMFunctionType(...)`) instead of the
`…InContext(codegen->context)` forms. The module lives in `codegen->context`, so the
declared `goo_make_chan`/`goo_chan_send`/`goo_chan_recv` function types — and the values
flowing into the calls — end up in a *different* LLVM context. That produces both
"Function context does not match Module context" and "Call parameter type does not match"
(a `ptr` in one context is not the same type object as a `ptr` in another). This is the
same defect class M3 fixed for match guards (global `LLVMAppendBasicBlock` →
`LLVMAppendBasicBlockInContext`).

---

## 2. Locked scope decisions

1. **In-process, buffered, single-goroutine round-trip only.**
   `make_chan(T, n)` → `ch <- v` → `v := <-ch` working without threads.
   Rejected: `go` spawning + unbuffered blocking rendezvous + `select` — they depend on the
   (separately unverified) threading/scheduler runtime; deferred to M8.

2. **Codegen-only fix.** Replace global-context LLVM builders with
   `…InContext(codegen->context)` in the channel codegen. The runtime and type checker are
   already correct.
   Rejected: runtime or type-system changes — the defect is purely in codegen's context use.

3. **Keep the existing `make_chan(T[, n])` builtin surface.**
   Rejected (deferred to a Go-compat follow-up): Go's `make(chan T, n)` syntax and
   comma-ok receive `v, ok := <-ch` — neither exists today; adding them is separate surface
   work, not part of fixing the measured bug.

4. **No new grammar.**

---

## 3. Architecture / touchpoints

| Stage | File | Work |
|---|---|---|
| Codegen — send/recv | `src/codegen/lowlevel_codegen.c` (`codegen_generate_channel_send`, `codegen_generate_channel_recv`) | use `codegen->context` for all types/constants; declare `goo_chan_send`/`goo_chan_recv` with in-context signatures matching the runtime (`int(goo_channel_t*, void*)`) |
| Codegen — make | `src/codegen/call_codegen.c` (`codegen_generate_make_chan_call`) | use `codegen->context`; declare `goo_make_chan` in-context (`ptr(size_t, size_t)`) |
| Codegen — channel type | `src/codegen/type_mapping.c` (`codegen_get_channel_type`) | build the opaque channel pointer type in `codegen->context` |
| Runtime | `src/runtime/channels.c` | **no change** — `goo_make_chan`/`goo_chan_send`/`goo_chan_recv` already implement a buffered ring buffer |
| Type checker | `src/types/expression_checker.c` | **no change** — `type_check_make_chan_call` already types it |

No new files. No grammar/runtime/type-system changes.

---

## 4. Element-size correctness

`make_chan(T, n)` sets the channel's `elem_size` from `T`; `goo_chan_send`/`recv` copy
`elem_size` bytes through the passed `void*` data pointer. The current `send` codegen
hardcodes an unused `sizeof(int)` local — the actual copy size comes from the channel, not
the codegen — but the value must be passed via a correctly-typed temporary so the runtime
reads/writes the right bytes. The probe therefore exercises a **non-`int` element** (an
`int64` and/or a small struct) so any lingering `int`-width assumption fails loudly.

---

## 5. Probe design — `chan-probe`

A single probe with exact-value assertions, FAILing before the fix (verifier error) and
PASSing after:

1. **Round-trip:** `ch := make_chan(int, 1); ch <- 42; x := <-ch; print(x)` → `42`.
2. **Buffered FIFO order (capacity 2):** send `1` then `2`; receive → `1` then `2`.
3. **Non-int element:** an `int64` (and/or a small struct) round-trip → exact value, to
   catch element-size assumptions.

All single-goroutine (buffered, no threads). Output is fixed and diffed via the established
harness.

---

## 6. Spike deliverable (records ground truth for the dataflow/NPU thesis)

Task 1 begins with a discovery spike that, beyond confirming the context fix, **documents in
the PR/spec whether `src/runtime/channels.c` is in-process only** — i.e. whether the
`endpoint` string on a channel drives any real transport or is merely stored. Expected
finding: buffered channels are an in-process ring buffer; networked endpoints (the
"on-chip ≡ off-chip dataflow" property) are **not** implemented and remain a separate, large
milestone. This is recorded so downstream architecture decisions rest on fact, not on the
presence of an `endpoint` field.

---

## 7. Testing / CI gates

- `chan-probe` added to BOTH `verify:` (Makefile) and `.github/workflows/tests.yml:54`.
- Full pre-existing probe gate stays green (26 CI probes on `main`+M6; 24 if M7 lands before
  M6 — the count is whatever the current `tests.yml` list holds, plus `chan-probe`).
- `opt --passes=verify` clean on `chan-probe`'s IR (the authoritative check — the original
  bug was an IR-validity failure).
- `make test` (unit suite) shows no new failures.
- Verified LOCALLY, since CI on `dd0wney` may be billing-blocked (jobs show red but never
  start).

---

## 8. Known limitations / deferred work

- **M8:** `go` spawning, unbuffered blocking rendezvous, `select` — real concurrency atop a
  working scheduler.
- **M9:** pattern channels (`pub`/`sub`/`req`/`rep`/`push`/`pull`) — wire codegen to
  `goo_make_pattern_chan` + `subscribe`/`pair_req_rep`/`add_worker`.
- **Separate (large):** networked endpoints / real transport (the distributed-dataflow
  property).
- **Go-compat follow-ups:** `make(chan T, n)` syntax; comma-ok receive `v, ok := <-ch`.
- The concurrency demos (`structured_concurrency_demo.goo`, `transparent_async_demo.goo`,
  etc.) stay aspirational.

---

## 9. Success criteria

M7 is complete when:
1. `chan-probe` compiles, runs, and diff-matches its expected output (buffered round-trip,
   FIFO order, non-int element).
2. `opt --passes=verify` is clean on the probe's IR.
3. It is wired into `tests.yml` and `make verify`.
4. The full pre-existing probe suite remains green (no regressions).
5. The endpoint-transport reality is documented (spike deliverable, §6).
6. Verified locally (CI may be billing-blocked).
