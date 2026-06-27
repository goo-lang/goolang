# M6 — Block-Scoped Value-Table Teardown (codegen correctness)

**Status:** Design (approved 2026-06-27)
**Milestone:** M6
**Predecessor:** M5 (Error Unions `!T`, string-error core) — shipped, `main` @ `84947a7` (PR #25)
**Theme:** Codegen correctness — fix a pre-existing lexical-shadowing miscompile so
inner-block variable bindings stop leaking past their scope. Small, single-task,
verify-and-fix. Clears the foundation before the next error-handling feature (`try`).

---

## 1. Problem

The codegen value table (`src/codegen/codegen.c`) is a flat array of `ValueInfo*`.
It is torn down only at function enter/exit (`codegen_enter_function` /
`codegen_exit_function`, which snapshot/restore `value_table_function_start`) and per
match arm (`composite_codegen.c:538/650`, which snapshot/restore `value_table_size`).
**Plain `{ }` blocks never tear down their bindings.** Combined with the M5
FIFO→LIFO lookup change (`codegen_lookup_value`, `codegen.c:321`), a variable
redeclared in an inner block leaks past that block:

```go
x := 1
if x == 1 {
    x := 2
    fmt.Println(x)   // 2  (correct)
}
fmt.Println(x)        // prints 2 — WRONG; Go semantics is 1
```

This is **pre-existing**, surfaced (not caused) by M5's review. Under the old FIFO the
same program printed `1, 1` (the inner use was wrong instead); both regimes are broken
because there is no block-scoped teardown. LIFO is the correct lookup order *given* a
scope stack — this milestone supplies the missing teardown so LIFO becomes fully
correct.

The type checker already scope_push/pops around constructs (`type_checker.c` has many
`scope_push`/`scope_pop` pairs), so this is a **codegen-only** mismatch: codegen's flat
table must match the checker's scoping.

---

## 2. Fix (single-point)

In `codegen_generate_block_stmt` (`src/codegen/statement_codegen.c:123`): snapshot
`value_table_size` at block entry and restore it at block exit, mirroring the existing
match-arm pattern:

```c
size_t pre_block_vt_size = codegen->value_table_size;   // before iterating statements
// … generate each statement …
codegen->value_table_size = pre_block_vt_size;          // restore on the way out
```

Why a single point is sufficient and safe:
- **Coverage:** if/for/while/catch/if-let bodies are all `AST_BLOCK_STMT` routed through
  `codegen_generate_block_stmt` via `codegen_generate_statement`, so one teardown covers
  every block-bodied construct.
- **Composition with existing scoping:**
  - **if-let** binds its unwrapped variable *before* generating the then-block (the
    binding is added at the if-let level, so it sits below the block's snapshot and
    survives the restore; if-let then does its own cleanup). No conflict.
  - **match arms** already snapshot/restore `value_table_size`; the arm body block now
    also snapshots/restores — nested and idempotent. No conflict.
  - **function params** are added before the body block's snapshot, so they survive.
- **Early-terminator path:** the existing `codegen_generate_block_stmt` early-`break`
  (when the current block already has a terminator, e.g. an if-let whose branches both
  return) must still restore `value_table_size` on the way out — place the restore so it
  runs on every exit path, including the `break` and the error `return 0`.

---

## 3. Scope (locked decisions)

1. **Codegen-only.** The type checker already scopes blocks; the fix makes codegen agree.
   The spike confirms the checker scopes plain blocks before relying on this.
   Rejected: a type-checker change (not where the miscompile lives).
2. **Single-point in `codegen_generate_block_stmt`.**
   Rejected: per-construct snapshot/restore in each of if/for/while codegen — more code,
   easy to miss a construct, no benefit over the single choke point.
3. **Correctness only — do NOT free the truncated `ValueInfo*`.** Mirror the existing
   match-arm/`exit_function` behavior (reset size without freeing). The small
   pre-existing leak (truncated entries never freed) is a documented separate follow-up,
   not part of this milestone.
   Rejected: also freeing on teardown — touches more, and a double-free is possible if
   ownership is subtle; deferred.
4. **No new grammar.**

---

## 4. Architecture / touchpoints

| Stage | File | Work |
|---|---|---|
| Codegen | `src/codegen/statement_codegen.c` (`codegen_generate_block_stmt`, ~line 123) | snapshot `value_table_size` at entry; restore on EVERY exit path (normal end, early terminator `break`, error `return 0`) |
| (spike-verify) | `src/types/type_checker.c` | confirm the checker scope_push/pops around plain block statements so codegen matches |

No new files. No runtime, grammar, or type-system changes.

---

## 5. Probe design — `block-scope-probe`

A single probe asserting Go scoping semantics, with exact expected output:

1. **Inner-block redeclare doesn't leak:** `x := 1; if x == 1 { x := 2; print(x) /*2*/ }; print(x) /*1*/`.
2. **Nested blocks:** a redeclare two levels deep restores correctly at each level.
3. **Loop-body redeclare resets each iteration:** a `for` whose body declares a local each
   iteration, observably independent across iterations.
4. **Regression guards:** an `if let` (M4) and a `match` (M3) binding still resolve
   correctly (the bound variable is usable in-block; outer names unaffected after).

Expected output is fixed and diffed via the established harness. The probe must FAIL
before the fix (printing the leaked inner value) and PASS after.

---

## 6. Testing / CI gates

- `block-scope-probe` added to BOTH `verify:` (Makefile) and `.github/workflows/tests.yml:54`.
- Full pre-existing probe gate (24 CI probes after M5) stays green — special attention to
  the if-let (`nullable-iflet-probe`), match (`match-probe`, `guard-probe`), and error-union
  catch probes, since the teardown touches the shared block path they rely on.
- `opt --passes=verify` clean on the new probe.
- Verified LOCALLY (`make test` + probe list + `opt verify`), since CI on `dd0wney` may be
  billing-blocked (jobs show red but never start).

---

## 7. Known limitations / deferred work

- **Value-table leak tidy-up:** the truncated `ValueInfo*` are not freed on teardown
  (block restore, `codegen_exit_function`, match arms) — a small per-scope leak, no
  use-after-free. Separate follow-up.
- Signedness-blind width extension across the nullable/error-union/struct-literal wrap
  sites (carryover from M4/M5) — separate follow-up.
- Larger error-handling milestones (`try` propagation, typed/enum errors) — later cycles.

---

## 8. Success criteria

M6 is complete when:
1. `block-scope-probe` compiles, runs, and diff-matches its expected output (Go scoping
   semantics: inner-block redeclarations do not leak).
2. It is wired into `tests.yml` and `make verify`.
3. The full pre-existing probe suite (24 CI probes) remains green — no regression to
   if-let, match, or catch block handling.
4. `opt --passes=verify` is clean on the new probe.
5. Verified locally (CI may be billing-blocked).
