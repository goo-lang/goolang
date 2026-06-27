# M5 — Error Unions (`!T`), String-Error Core: Verify-and-Fix

**Status:** Design (approved 2026-06-27)
**Milestone:** M5
**Predecessor:** M4 (Nullable Types `?T`, safe core) — shipped, `main` @ `49c765a` (PR #24)
**Theme:** Error handling, phase 2 — the `!T` (error-union) half. Pairs with M4's
`?T` nullable half. `try` propagation and typed/enum errors are deferred to M6+.

---

## 1. Summary

Goo's frontend already contains extensive error-union scaffolding (`!T` type,
`try`/`catch` grammar, AST nodes, type-checker handling, and codegen helpers) that
has **never been proven to compile and run** — no probe exercises it. M5's job is to
make the **string-error core** of `!T` actually work end-to-end, probe-gated, sliced
into M4-style atomic tasks. This is *verify-and-fix*, not greenfield. No new grammar.

### What already exists (the real `make goo` pipeline)
- **Grammar:** `error_union_type: BANG type` (`!T`); `catch_expr: expression CATCH
  identifier block` (`expr catch e { … }`); `try_expr: TRY expression` (deferred).
- **AST:** `AST_ERROR_UNION_TYPE` (`ErrorUnionTypeNode{value_type, error_type?}`),
  `AST_CATCH_EXPR` (`CatchExprNode{expr, error_var, catch_body}`), `AST_TRY_EXPR`.
- **Type system:** `TYPE_ERROR_UNION` (value_type + optional error_type),
  `type_error_union()`, `type_from_ast` for `AST_ERROR_UNION_TYPE`.
- **Type checker:** `type_check_catch_expr` requires an error-union, binds `error_var`
  as **`TYPE_STRING` by default** (`expression_checker.c:825`), and types the `catch`
  expression as the **unwrapped value type `T`** (`expression_checker.c:839`).
- **Codegen:** `codegen_get_error_union_type` builds `{ i1 is_error, { value, error } }`
  (default error slot `i8*`); `codegen_create_error_union_value`,
  `codegen_extract_error_union_value`, `codegen_check_error_union`;
  `codegen_generate_error_union_function`; `catch`/`try` dispatch in
  `expression_codegen.c`.

### The gap
Zero probes → none of it is verified end-to-end, and there is **no error-construction
mechanism at all** (nothing produces the *error* case). Per the recurring Goo pattern
(rich scaffolding, never run — the reason M3/M4 existed), the work is: write probes →
discover what actually breaks → fix typecheck/codegen until green.

---

## 2. Locked scope decisions

Decisions made during brainstorming (with rejected alternatives recorded):

1. **Error model = string message.**
   `!T` is a value *or* an error carrying a **message string** (the scaffold's default
   `i8*` error slot → Goo `string`). The type checker already defaults the `catch`
   variable to `TYPE_STRING`, confirming this grain.
   Rejected: *int error code* (too weak — no message/identity); *typed/enum errors with
   payloads* (the flagship, but largest — needs the error type threaded through grammar,
   which today names only the value type, plus `catch`↔`match` interplay; deferred to M6).
   Rejected: *a Go-style `error` interface* (Goo has no `error` interface; would require
   interface work — too big, not verify-and-fix).

2. **Scope = core: declare + construct + `catch`. `try` deferred.**
   Rejected: *full (+`try` propagation)* — `try` threads early-return control flow
   through the enclosing function's error-union return; riskier, better once the
   representation is proven. Rejected: *propagation-first* — `try` alone isn't usable
   (something must still `catch`). `catch` (explicit handling) is also the more
   Go-faithful primitive (Go's norm is `if err != nil`, not propagation).

3. **Error construction = `error("msg")` builtin + success-value auto-wrap. No new grammar.**
   `return value` in a `!T` function auto-wraps the success case; `return error("msg")`
   produces the error case. `error` is a builtin (`string → !T` in context), parsed as
   an ordinary call (no grammar change).
   Rejected: *polymorphic bare return* (`return <string>`=error vs `return <T>`=success)
   — ambiguous for `!string`, fragile. Rejected: *a new `fail "msg"` keyword* — requires
   a grammar change, against the no-new-grammar discipline that served M3/M4.

4. **Work structure = layered probes per capability (M4 Approach B).**
   Each capability gets its own probe + atomic task + reviewer gate. A nullable/error
   value cannot be *observed* without a handling primitive, so Task 1 bundles
   construct-success with `catch` (the observation primitive), exactly as M4 bundled
   construct with `if let`.

---

## 3. Architecture / pipeline touchpoints

M5 touches only where the error-union path is already plumbed — no new subsystems:

| Stage | File(s) | Expected work |
|---|---|---|
| Lexer/grammar | `src/parser/*.y` | **Verify only** — `!T`, `catch`, and `error(...)` (as a call) already parse. |
| Type checker | `expression_checker.c`, `type_checker.c` | Recognize `error(string)` as a builtin yielding the error case of the contextually-expected `!T`; ensure `return v` / `return error(...)` type-check inside a `!T` function. `catch` already typed. |
| Codegen | `type_mapping.c`, `function_codegen.c`, `expression_codegen.c` | Wire success auto-wrap, the `error()` builtin construction, and `catch` branching. Apply M4's centralized integer width-adjust to the value slot. |
| Runtime | — | **None** — error-union is a value struct; the message is a `string`/`i8*`. |

---

## 4. Representation & ABI

- `!T` lowers to `{ i1 is_error, { T value, string msg } }` (existing
  `codegen_get_error_union_type`; default error slot is `i8*` = string).
  - success → `{ is_error=false, { value=v, msg=undef } }`
  - error → `{ is_error=true, { value=zero, msg="…" } }`
- **ABI rule:** the same >16-byte-by-pointer rule as M4/slices; per M4's finding, LLVM's
  backend applies the by-pointer convention automatically once the IR is correctly typed.
  Task 3 exercises `!BigStruct`.
- **Integer width:** reuse M4's centralized width-adjust (the `?T` fix lives in
  `codegen_create_nullable_with_value`; the error-union value-wrap path must apply the
  same SExt/Trunc-to-slot-type so e.g. `!int64` returning an i32 literal is well-typed).
  Task 3 exercises `!int64`.

---

## 5. Surface & semantics (exact, locked)

| Construct | Semantics |
|---|---|
| `func f() !T` | declares an error-union return |
| `return v` (v : T) | success → `{is_error=false, value=v}` (auto-wrap) |
| `return error("msg")` | failure → `{is_error=true, msg="msg"}`; `error` is a builtin `string → !T` resolved against the expected return type |
| `x := expr catch e { … }` | `expr : !T`. **ok:** `x = value` (type `T`). **error:** bind `e : string` in the block scope, execute the block; the block typically diverges (`return`); if it falls through, `x` = the zero value of `T` (total, well-defined) |

**Out of scope (M6+):** `try` propagation; typed/enum errors and payloads; custom error
types; error context/hierarchy (`@error_hierarchy`); matching on error kind.

---

## 6. Probe design (layered) + task slicing

Three atomic tasks, each ending in a green CI probe added to
`.github/workflows/tests.yml` (the probe list ~line 54) **and** `make verify`. Each
probe is a `.goo` source plus a checked-in `.expected.txt`, diffed via the established
harness. Each task begins with a discovery spike (the scaffolding has never run).

| # | Task | Probe | Covers |
|---|---|---|---|
| 1 | Declare + success + `catch` (ok path) | `erru-catch-probe` | `func f() !int` returning `return v`; `x := f() catch e { … }` yields the value; print `x`. Foundational (an `!T` is unobservable without `catch`). |
| 2 | `error()` builtin + `catch` error path | `erru-error-probe` | `return error("msg")`; `x := g() catch e { print e; <fallback> }` (error path binds `e`, prints the message, yields a fallback); plus a `catch { … return }` divergence case. |
| 3 | ABI + width | `erru-abi-probe` | `!BigStruct` (>16 B) returned across functions; `!int64` value-slot width-adjust (reuse M4's centralized fix). The high-risk corner, isolated for its own review gate. |

Each task: discovery spike → fix typecheck/codegen → probe green → wire CI → commit.

---

## 7. Testing / CI gates

- Three new probes wired into the `tests.yml` probe list and `make verify`.
- **Adversarial whole-branch review** (the M3/M4 step that caught real miscompiles).
  Explicit negative/edge cases:
  - error-path **zero-value fallthrough** (catch block that doesn't diverge).
  - `catch`-with-`return` **terminator integrity** (M4 had if-let/return terminator bugs).
  - `!int64` value-slot **width** and `!BigStruct` **ABI**.
  - `error("")` **empty message**.
- `make verify` halts at `ccomp-build` (CompCert env gap) — the real gate is the explicit
  CI probe list, per project build facts.
- **CI note:** GitHub Actions on `dd0wney/goolang` may be billing-blocked (jobs show red
  but never start). When blocked, the authoritative verification is local: `make test`
  (unit suite) + the probe list + `opt --passes=verify` on emitted IR.

---

## 8. Known limitations / deferred work (documented, not bugs)

- **M6+ candidates:** `try` propagation (`try expr` → unwrap-or-early-return from an
  enclosing `!U`); typed/enum errors with payloads; custom error types; error
  context/hierarchy; matching on error kind.
- `examples/ergonomic_error_handling_demo.goo` uses the deferred surface (`try`, custom
  error enums, `@error_hierarchy`) — it stays **aspirational**; annotate it as a later
  milestone target so it is not mistaken for a regression.
- Carryover from M4: struct-field default-nil for `?T` fields (separate, not M5).

---

## 9. Success criteria

M5 is complete when:
1. All three probes (`erru-catch-probe`, `erru-error-probe`, `erru-abi-probe`) compile,
   run, and diff-match their expected output.
2. All three are wired into `tests.yml` and `make verify`.
3. The full pre-existing probe suite (21 CI probes after M4) remains green (no regressions); the 3 new probes bring the CI list to 24.
4. The `!BigStruct` ABI and `!int64` width cases are exercised and passing.
5. Deferred surface is documented; the ergonomic demo annotated as a later target.
6. Verified locally (`make test` + probe list + `opt --passes=verify`), since CI may be
   billing-blocked.
