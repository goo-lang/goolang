# M4 — Nullable Types (`?T`), Safe Core: Verify-and-Fix

**Status:** Design (approved 2026-06-27)
**Milestone:** M4
**Predecessor:** M3 (Foundation Hardening) — shipped, `main` @ `ed8f887`
**Theme:** Error handling, phase 1 — the `?T` (nullable / optional) half of Goo's
error-handling story. The `!T` error-union + `try`/propagation half is deferred to M5.

---

## 1. Summary

Goo's frontend already contains substantial nullable-type *scaffolding* that has
**never been proven to compile and run** — there is no probe exercising it. M4's job
is to make the **safe core** of `?T` actually work end-to-end, probe-gated, sliced
into M3-style atomic tasks. This is *verify-and-fix*, not greenfield: no new grammar.

### What already exists (the real `make goo` pipeline)
- **AST:** `AST_NULLABLE_TYPE` (`?T`), `if-let` statement node (`IfLetStmtNode`).
- **Grammar:** `nullable_type: QUESTION type`; `if_let_stmt: IF LET identifier ASSIGN
  expression block [ELSE …]`. `nil` literal exists.
- **Type system:** `TYPE_NULLABLE` (base_type), `type_nullable()` constructor,
  equality, `type_from_ast` for `AST_NULLABLE_TYPE`; `nil` assignable to nullable;
  `if-let` type-checked (requires a nullable, binds the unwrapped base type).
- **Codegen:** `nullable_codegen.c` exports `create_nullable_with_value`,
  `create_nullable_null`, `nullable_is_null`, `nullable_get_value`, and a full
  `codegen_generate_if_let_nullable`; `statement_codegen.c` dispatches
  `AST_IF_LET_STMT` with real basic-block lowering (`iflet.then/else/merge`).

### The gap
Zero probes → none of the above is verified end-to-end. Per Goo's recurring
"rich scaffolding, never proven" pattern (the reason M3 existed), the work is:
write probes → discover what actually breaks when the code first runs → fix
typecheck/codegen until green.

---

## 2. Locked scope decisions

Decisions made during brainstorming (with rejected alternatives recorded):

1. **Theme = error handling; M4 = nullable-only.**
   Rejected: generics/interfaces (larger, riskier, scaffolding least likely wired);
   error-union-first (bigger differentiator but force-unwrap/propagation needs a
   runtime-trap design first). Nullable is the closest-to-finished, lowest-risk slice
   and proves the additive-type-feature pipeline before tackling `!T`.

2. **Safe-only access surface.**
   Rejected: the `nullable_types_demo.goo` surface (`if user? {…}` presence test +
   `user!` force-unwrap). Force-unwrap requires runtime null-trap/panic machinery that
   may not exist yet and overloads `!` (already error-union prefix + logical-not).
   Deferred to M5 as a deliberate decision.

3. **Verify-and-fix only — no new grammar.**
   Rejected: adding the coalesce operator `x ?? default` in M4. Adding new grammar on
   top of an unverified core would conflate "new feature broke" with "old scaffolding
   was already broken." `??` is cleanly addable in M5 once the core is proven.

4. **Work structure = layered probes per capability (Approach B).**
   Rejected: a single coarse end-to-end probe (Approach A — can't bisect which feature
   broke); a separate discovery-spike phase (Approach C — over-formalizes; the spike is
   folded into Task 1 instead). Layered probes match the repo's 21-probe granularity
   and give clean atomic tasks for subagent-driven development.

---

## 3. Architecture / pipeline touchpoints

M4 touches only where the nullable path is already plumbed — no new subsystems:

| Stage | File(s) | Expected work |
|---|---|---|
| Lexer/grammar | `src/parser/*.y` | **Verify only** — `?T`, `if let`, `nil` already parse. |
| Type checker | `type_checker.c`, `expression_checker.c` | Fix candidates: `x == nil` / `x != nil` comparison typing; auto-wrap of a `T` value into `?T` on assignment/return/argument. |
| Codegen | `nullable_codegen.c`, `statement_codegen.c`, `expression_codegen.c` | Fix candidates: construction sites (auto-wrap); nil-compare lowering; the by-pointer ABI path for large base types. |
| Runtime | — | **None expected** — nullable is a pure value struct; no runtime calls. |

---

## 4. Representation & ABI

- `?T` lowers to LLVM struct `{ i1 is_null, T value }` (already chosen in
  `nullable_codegen.c`).
  - `nil` → `{ is_null = true, value = undef }`
  - a value `v : T` → `{ is_null = false, value = v }`
  - default-zero of a `?T` variable/field is `nil`.
- **ABI rule (locked):** when `sizeof({i1, T}) > 16 bytes`, the value must cross
  codegen↔C / function-call boundaries **by pointer, not by value** — per the
  `m12_probe` / slice-ABI precedent (runtime/codegen structs >16 bytes by pointer).
  Therefore the probe suite **must** exercise `?BigStruct` returned from *and* passed
  to a function, not only `?int`. This is the highest-risk corner and is a first-class
  test target, not an afterthought.
- `is_null` is an in-struct `i1`; `value` follows natural alignment. If the existing
  helpers assumed a packed layout, fixing alignment is a candidate task finding.

---

## 5. Feature surface & semantics (exact, locked)

| Construct | Semantics |
|---|---|
| `var x ?T`; struct field `f ?T`; return type `?T`; param `?T` | declares nullable; default value is `nil` |
| `x = v` where `v : T` | auto-wrap → `{ is_null=false, value=v }` |
| `x = nil` | `{ is_null=true, value=undef }` |
| `if let y = x { … } else { … }` | if `!x.is_null`: bind `y := x.value` (type `T`) in then-block; `else` optional |
| `x == nil`, `x != nil` | read `is_null` → `bool` |

**Out of scope (M5+):** `x!` (force-unwrap), `x?` (presence test), `x ?? d`
(coalesce), nested/chained auto-unwrap, `!T` (error union), `try`/propagation.

---

## 6. Probe design (layered) + task slicing

Three atomic tasks, each ending in a green CI probe added to `.github/workflows/tests.yml`
(the probe list ~line 54) **and** `make verify`. Each probe is a `.goo` source plus a
checked-in `.expected.txt`, diffed via the established harness.

| # | Task | Probe | Covers |
|---|---|---|---|
| 1 | Declare / construct / nil + ABI | `nullable-decl-probe` | declare `?int` **and** `?BigStruct`; assign value; assign `nil`; **return from and pass to a function** (ABI by-pointer for the big struct); print a sentinel. **Begins with a throwaway discovery spike** to map what actually breaks. |
| 2 | `if let` unwrap | `nullable-iflet-probe` | `if let` with and without `else`, over `?int` and `?BigStruct`; bound value usable in then-block. |
| 3 | nil comparison | `nullable-nilcmp-probe` | `x == nil` / `x != nil` in `if` conditions and as plain bool values; both null and non-null operands. |

Each task: discovery spike (local) → fix typecheck/codegen → probe green → commit →
one PR off updated `main` (M3 cadence). Tasks are ordered (2 and 3 build on 1's
construction path) but each leaves the tree green.

---

## 7. Testing / CI gates

- Three new probes wired into the `tests.yml` probe list and `make verify`.
- **Adversarial whole-branch review per task** (the M3 step that caught two real
  miscompiles). Explicit negative/edge cases to probe:
  - `nil` big-struct returned across a function boundary (ABI).
  - `if let` on an already-`nil` value taking the `else` branch.
  - double-assign `value → nil → value` to the same nullable.
  - `x == nil` immediately after `x = nil`, and after `x = v`.
- `make verify` halts at `ccomp-build` (CompCert env gap) — the real gate is the
  explicit CI probe list, per project build facts.

---

## 8. Known limitations / deferred work (documented, not bugs)

- **M5 candidates:** force-unwrap `x!`, presence-test `x?`, coalesce `x ?? default`,
  error-unions `!T`, `try`/propagation.
- `examples/demos/nullable_types_demo.goo` uses the deferred `x!`/`x?` surface — it
  stays **aspirational**; annotate it as an M5 target so it is not mistaken for a
  regression when it fails to compile.
- Strict cross-type assignment rigor (carryover from M3's lenient type checker) is out
  of scope here.

---

## 9. Success criteria

M4 is complete when:
1. All three probes (`nullable-decl-probe`, `nullable-iflet-probe`,
   `nullable-nilcmp-probe`) compile, run, and diff-match their expected output.
2. All three are wired into `tests.yml` and `make verify`, and CI is green.
3. The full pre-existing probe suite (21 probes) remains green (no regressions).
4. The large-struct nullable ABI case is exercised and passing.
5. Deferred surface is documented; `nullable_types_demo.goo` annotated as M5.
