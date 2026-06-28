# M8b — Conservative Heap-Promotion of Goroutine-Escaping Locals

**Status:** Design (approved 2026-06-28)
**Milestone:** M8b (Increment B of multi-arg goroutine support)
**Predecessor:** M8 concurrency substrate (goroutine spawning + arg thunking, unbuffered
channels, blocking `select`) — PR #29 (`dd0wney/goolang`), which also introduced the
`goo_scheduler_wait()` run-to-completion barrier at generated `main` exit.
**Theme:** Memory safety for goroutine arguments — close the one real dangling-pointer
hazard that survives M8's allocate-and-leak model and the #29 barrier.

---

## 1. Summary

M8's argument thunking (`go f(a, b)` heap-boxes evaluated args into a per-call-site
struct and a `__goo_thunk_N` that unboxes/calls/frees) is type-agnostic: it copies each
argument *value* into the box. For reference-typed args the box copies a header/pointer,
not the pointed-to backing data. The concern raised during design — "a goroutine reads
backing data that was freed before it ran" — turns out to be **already neutralized for
almost every case** by two existing facts:

1. **Allocate-and-leak.** Slice-literal backing is `goo_alloc`'d and never freed
   (`src/codegen/composite_codegen.c:702-708`, explicitly). String literals are immortal
   globals (`LLVMBuildGlobalStringPtr`). Heap data that is never freed cannot dangle.
2. **The #29 run-to-completion barrier.** Generated `main` calls `goo_scheduler_wait()`
   before returning, so any local owned by `main` — even `&stackLocal` — stays alive for
   the full lifetime of every goroutine `main` spawns.

After mapping the hazard surface, exactly **one** real present-day dangling hazard remains,
and M8b closes it.

---

## 2. The hazard (exact)

`&stackLocal` (the address of a stack-allocated local, or any interior pointer
`&local.field` / `&local[i]`) passed into a `go` call **from a non-`main` function that
returns before the goroutine runs.**

- `&x` yields the raw stack `alloca` address (`src/codegen/expression_codegen.c:932-944`),
  with no escape-promotion.
- The #29 barrier only keeps `main`'s frame alive; intermediate function frames are popped
  on return, so a goroutine reading through such a pointer reads freed stack memory.

Minimal reproducer:

```go
package main
import "fmt"

func reader(p *int, done chan int) {
    done <- *p            // reads through the (would-be dangling) pointer
}

func spawn(done chan int) {
    x := 7
    go reader(&x, done)   // &x escapes into the goroutine
}                         // spawn returns; x's stack slot is gone

func main() {
    done := make_chan(int, 1)
    spawn(done)
    fmt.Println(<-done)   // without promotion: garbage; with: 7
}
```

(Read-through-pointer is sufficient to exercise the hazard and depends only on pointer
deref-load, which codegen already supports; deref-*store* is intentionally avoided so the
probe does not gate on an unverified feature.)

### Latent (out of scope, tracked)
The moment Goo introduces *any* heap reclamation (freeing `goo_alloc`'d memory), every
currently-safe slice/string case becomes a dangling hazard. M8b's pass is the foundation
that a future reclamation milestone builds on, but precise lifetime/freeing is **not**
solved here.

---

## 3. Locked scope decisions

1. **Conservative heap-promotion, not rejection, not precise analysis.** Go-faithful:
   `go f(&x)` must just work, matching Go's escape analysis. Rejected: a typecheck
   rejection of stack-pointer `go` args (safe and trivial but not Go-faithful — rejects a
   legal Go pattern). Rejected: precise def-use/alias analysis (most faithful, no
   over-promotion, but the largest and most error-prone — real pointer analysis, premature).

2. **"Address-taken" is the complete, sound trigger.** A pointer into a local's storage
   cannot exist without `&` somewhere, so detecting the `&` operation itself covers the
   aliased case (`p := &x; go f(p)`) for free: we see `&x`, promote `x`, and every use of
   `x` — including through `p` — then references heap. No pointer-flow tracking needed.

3. **Promotion is scoped to functions containing a `go`.** A `&local`-without-`go` is not
   a goroutine hazard (a function returning `&local` is a separate, pre-existing bug),
   so M8b leaves all non-`go` functions untouched.

4. **Over-promotion is accepted.** A `&`-taken local in a `go`-containing function is
   heap-promoted even if it does not actually reach the `go`. Safe, leak-consistent, and
   `go`-containing functions are rare. Documented, not optimized.

5. **Leak-consistent allocation.** Promoted locals use `goo_alloc` and are never freed,
   matching the existing allocate-and-leak model. No runtime change.

6. **No grammar change. No runtime change.** Codegen-only (a pre-pass + the allocation
   site), mirroring the M6/M7/M8 codegen-fix pattern.

---

## 4. The promotion rule

A local `L` in function `F` is heap-promoted iff **both**:

- **(a)** `L`'s address is taken anywhere in `F` — `&L`, or an interior address
  `&L.field` / `&L[i]` whose root lvalue is `L`; **and**
- **(b)** `F` contains at least one `go` statement (`AST_GO_STMT`).

Soundness: (a) is the only way a pointer into `L`'s storage can come to exist; (b) scopes
the change to functions that can leak a local into a goroutine. Any pointer that could
reach a `go` is therefore backed by heap storage.

---

## 5. Architecture / touchpoints

| Stage | File | Work |
|---|---|---|
| Per-function pre-pass | `src/codegen/function_codegen.c` (before emitting `F`'s body) | walk `F`'s AST: set `hasGo` if any `AST_GO_STMT`; collect `addrTakenRoots` = root locals of any address-of lvalue. If `hasGo`, the promotion set = `addrTakenRoots`. |
| Local allocation site | local/`var`/`:=` declaration codegen (where `codegen_create_entry_alloca` is used for locals) | if the local is in the promotion set, allocate via `goo_alloc(sizeof T)` instead of a stack `alloca`; store that pointer in the value table under the local's name. |
| Everything else | — | **unchanged** — loads, stores, and `&L` already operate through the value-table pointer, which is now heap instead of stack. |
| Runtime | — | **no change** (`goo_alloc` already exists and is declared on demand in codegen). |
| Grammar | — | **no change**. |

Implementation note (resolve during planning): confirm the exact local-allocation site
and the value-table name→storage mapping (`:=` short-decl and `var` paths), and the AST
walker's coverage of address-of forms (`AST_UNARY_EXPR` `&` over ident / field / index).

---

## 6. Why the probe must spawn from a non-`main` frame

`main`-spawned locals are already safe via the #29 barrier, so a `main`-only probe would
pass even *without* the fix. The probe therefore spawns from a helper function that
returns before the goroutine necessarily runs (see §2 reproducer). The channel receive in
`main` is the deterministic join: the cooperative scheduler runs the goroutine while
`main` blocks on `<-done`, by which point the helper's frame is gone — so the read is
deterministic garbage without promotion and a deterministic `7` with it.

---

## 7. Probe design — `escape-probe`

- `examples/escape_probe.goo`: the §2 reproducer, plus an aliased variant
  (`p := &x; go reader(p, done)`) to exercise scope decision #2.
- `examples/escape_probe.expected.txt`: the exact delivered values.
- Must **FAIL** before the fix (garbage / crash from the dangling read) and **PASS** after.
- IR assertions: the promoted local shows a `goo_alloc` call; a control function with `&`
  but **no** `go` still shows a stack `alloca` (scope decision #3).
- `opt --passes=verify` clean; run under `timeout`; wired into BOTH `verify:` (Makefile)
  and `.github/workflows/tests.yml`.

---

## 8. Testing / CI gates

- `escape-probe` added to `verify:` and `tests.yml`, under a timeout.
- Full pre-existing probe gate stays green — special attention to `go-probe` and the
  channel probes (M8b sits directly on the goroutine path).
- `make test` (unit suite) shows no new failures.
- Verified **locally** (CI on `dd0wney` may be billing-blocked — jobs show red but never
  start; local probe gate + `make test` are authoritative).

---

## 9. Known limitations / deferred work

- **Over-promotion** (scope decision #4): some non-escaping `&`-taken locals are
  heap-allocated. Safe; extra leak only.
- **Reclamation dependency** (§2 latent): when Goo adds heap freeing, every currently-safe
  slice/string goroutine arg becomes a hazard; M8b is the foundation but precise
  lifetime/freeing is a later milestone.
- **Precise escape analysis** (alias-exact promotion, no over-promotion) remains a possible
  future refinement.
- **Multi-threaded scheduler** is still `num_threads = 1` (M8 deferral); M8b's correctness
  does not depend on it.

---

## 10. Success criteria

M8b is complete when:
1. `escape-probe` compiles, runs (under timeout), and diff-matches expected output — a
   goroutine spawned from a non-`main` frame safely reads/writes a promoted local after the
   spawning frame has returned.
2. The aliased (`p := &x`) variant is covered and passes.
3. IR shows `goo_alloc` for a promoted local and a stack `alloca` for a `&`-taken local in
   a `go`-free control function.
4. `opt --passes=verify` is clean on the probe's IR.
5. It is wired into `tests.yml` and `make verify`.
6. The full pre-existing probe suite remains green (no regressions, esp. `go-probe` +
   channel probes).
7. Verified locally (CI may be billing-blocked).
