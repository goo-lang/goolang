# Checker/Codegen Hygiene Cluster Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the three #101-review backlog items, all reproduced on main @13cbf63: (T1) float `%` gives an opaque codegen error (`g % 2.0` → "Failed to generate binary operation"; Go rejects at typecheck: `invalid operation: operator % not defined on g (variable of type float64)`); (T2) `codegen_generate_binary_expr` re-calls `type_check_binary_expr` mid-codegen (expression_codegen.c:1210) — a redundant re-check that re-runs adaptation after operands are already emitted, clobbers literal stamps, and forced #102's LLVMTypeOf-based category-correction patch (the symptom fix); (T3) one rejected declaration cascades into 3-4 spurious errors (`var b int8 = 300` → "constant 300 overflows int8" + "Undefined variable 'b'" ×2 + "Invalid initializer expression" + "Undefined variable 'c'").

**Architecture:** (T1) A float-operand rejection in `type_check_arithmetic_op`'s `%` arm — smallest task, lands first, and removes a codegen-unreachable-error class before T2 touches codegen. (T2) The checker records its computed result type on the node (`expr->node_type`) on every successful return of `type_check_binary_expr`; codegen reads the recorded type instead of re-checking (NULL = loud `codegen_error` — the checker always runs first, so a missing recording is a compiler bug, not a user error). The #102 category-correction block is then re-derived: if the stamp-clobbering re-check was its only trigger, demote it to a defensive verify-and-error (or delete with an explanatory commit note) — evidence decides, not assumption. (T3) Error-recovery registration: when a var declaration's initializer fails its check, still register the variable with its DECLARED type (when one exists) so downstream uses resolve quietly; the one real error stands alone. `:=` declarations with failed RHS get investigated — register with the RHS type if the checker produced one before failing, otherwise record the finding and leave (do not invent a sentinel type system this branch).

**Tech Stack:** C23, LLVM-C. Checker (T1, T3) + checker/codegen pair (T2). No parser/header/Bison changes expected (T2 may touch `include/types.h` ONLY if a helper prototype needs it — then `make clean`).

## Global Constraints

- Branch: `fix/checker-hygiene` off main @13cbf63. Do NOT commit on main.
- Commits: conventional, imperative, `--no-gpg-sign`. Stage only named files; never stage `.superpowers/` or `.handoff.md`.
- Gate per task: `make lexer`, probes, then `eval "$(opam env --switch=default)"`, `make verify` (ALL PASS; golden 187/0 grows per probe; all 22 reject probes stay green) and `make test` (76/1). STOP/BLOCKED on any regression. **T2 is the risk task: it changes the result-type source for EVERY binary expression — the entire golden suite is the guard, and the adaptation/numeric net especially** (`const_expr_probe`, `cross_kind_probe`, `float_binop_probe`, `float_adapt_probe`, `int_width_probe`, `shift_width_probe`, `var_width_probe`, `composite_field_adapt_probe`, `nullable_adapt_probe`, `global_init_probe`, `float_fidelity_probe`).
- Go conformance: `go run`-verify probe expectations; record. Error-message wording follows the repo's established vocabulary (decision parity with Go, not byte-identical text — adjudicated in #103).
- Probe hygiene: bool-compare floats, exactly-representable values, same-width prints.
- Reject-probe pattern: mirror `consttrunc-reject-probe` exactly, wired into `verify`.
- Pre-commit hook runs `make test`.

## Reference: verified code landmarks (2026-07-03, main @13cbf63)

- Arithmetic op checker: `type_check_arithmetic_op`, `src/types/expression_helpers.c` (~:118-160; its comment says "type_check_binary_expr handles most of these earlier; this is the [backstop]"). The `%` arm currently returns a float type for float operands (no rejection) — codegen's TOKEN_MODULO arm is integer-only, hence the opaque error.
- Mid-codegen re-check: `src/codegen/expression_codegen.c:1210` — `Type* result_type = type_check_binary_expr(checker, expr);` inside `codegen_generate_binary_expr`, AFTER operand emission and the M3/float width coercions (:1199-1207). The category-correction patch it forced: ~:1507 (comment "from a REDUNDANT re-check of `expr` (the `type_check_binary_expr` call ...)").
- Result recording: `expr->node_type` is the AST's type-recording field; `type_check_binary_expr` (`src/types/expression_checker.c:1300+`) already sets it on SOME paths (e.g. the `_ =` arm :1316) — audit EVERY successful return path; the adapters set `node_type` on operand nodes during stamping (different concern — the binop's own result recording is what T2 needs).
- Cascade sources: "Undefined variable" at `src/types/expression_checker.c:605`; var registration in `type_check_var_decl` (`src/types/type_checker.c:800+`) — read its failure paths to see where registration is skipped on initializer-check failure; "Invalid initializer expression" is the `:=`/decl follow-on error.
- Repro (2026-07-03 @13cbf63): `g := 2.5; h := g % 2.0` → 2 opaque codegen errors. `var b int8 = 300; ...; c := b; ...` → 1 real + 4 spurious errors.
- Go reference: `go run` on float `%` → `invalid operation: operator % not defined on g (variable of type float64)`. Go's cascade behavior: exactly ONE error for the b/c program (`constant 300 overflows int8`) — b and c resolve with their declared/inferred types.

---

### Task 1: Reject `%` on float operands at the checker

**Files:**
- Modify: `src/types/expression_helpers.c` (`type_check_arithmetic_op` `%` arm)
- Test: `examples/floatmod_reject.goo` + Makefile target `floatmod-reject-probe`
- Modify: `Makefile` (target, wired into `verify`)

**Interfaces:** none consumed/produced; standalone.

- [ ] **Step 1: Reject probe** — `examples/floatmod_reject.goo`:
```go
package main

import "fmt"

func main() {
	g := 2.5
	h := g % 2.0
	fmt.Println(h)
}
```
Makefile `floatmod-reject-probe` mirroring `consttrunc-reject-probe` (rm -f, rc≠0, no binary, anti-crash grep, diagnostic grep `not defined on float`), wired into `verify` (→ 23 reject probes). Header: Go rejects too (record Go's exact message) — Go-CONFORMANT rejection.
- [ ] **Step 2: Verify today** — record the two opaque errors (RED).
- [ ] **Step 3: Implement** — in `type_check_arithmetic_op`'s `%` handling: if either operand type is float-kind, `type_error(..., "operator %% not defined on %s", <float type name>)` and return NULL. Cover float32 and float64. Check what `2.5 % 2` (untyped-float-rooted meets int) does after the cross-kind machinery — the #101 rejection block may already catch some shapes; record which path each shape takes: `g % g`, `g % 2.0`, `2.5 % g`, `g % 2` (all must reject SOMEWHERE with a clean message, none may reach codegen).
- [ ] **Step 4: Gate** — reject probe PASS; full golden 187/0 (no golden uses float %; if one does, STOP and report); test 76/1.
- [ ] **Step 5: Commit** — "fix(types): reject %% on float operands at typecheck (was opaque codegen error)".

---

### Task 2: Checker records binop result type; codegen stops re-checking

**Files:**
- Modify: `src/types/expression_checker.c` (`type_check_binary_expr` — set `expr->node_type` on every successful return)
- Modify: `src/codegen/expression_codegen.c` (:1210 re-call replaced by reading `expr->node_type`; category-correction block re-derived)
- Test: `examples/binop_stamp_probe.goo` + `.expected.txt`

**Interfaces:**
- Consumes: T1 (float `%` never reaches codegen).
- Produces: invariant "after a successful checker pass, every AST_BINARY_EXPR carries its result type in `node_type`" — document it at both ends with change-together comments.

- [ ] **Step 1: Probe** — `examples/binop_stamp_probe.goo`, locking the shapes the re-check previously mangled or the category fix guarded:
```go
package main

import "fmt"

func takesF32(x float32) float32 {
	return x
}

func main() {
	g := float32(2.5)
	h := (1 + 2) * g
	fmt.Println(h == float32(7.5))
	c := 2.0 * 3.0 * g
	fmt.Println(takesF32(c) == float32(15.0))
	i := (1 + 1) + 0.5
	fmt.Println(i == 2.5)
	x := int64(7)
	fmt.Println(x + 1 == 8)
	u := uint32(4000000000)
	fmt.Println(u/2 == 2000000000)
	fmt.Println((1+2)*3 == 9)
}
```
`.expected.txt`: `true` ×6. `go run`-verify. (These currently PASS via the category patch — the probe locks behavior across the mechanism swap; it is a regression net, not a RED test. Say so in its header.)
- [ ] **Step 2: Audit recording** — read every successful `return` in `type_check_binary_expr` (and any helper it delegates final returns to); list in the report which paths already set `expr->node_type` and which don't. Add `expr->node_type = <result>` where missing, immediately before each return (or refactor to a single exit if that simplifies without churn).
- [ ] **Step 3: Codegen swap** — replace :1210's re-call with `Type* result_type = expr->node_type;` + a loud `codegen_error(...)` on NULL ("binary expression not typed by checker — compiler bug"). Update the surrounding comment: the checker phase is the single source of typing; the mid-codegen re-check is gone (why: it re-ran adaptation after operand emission and clobbered literal stamps — cite #101/#102 review history briefly).
- [ ] **Step 4: Re-derive the category patch** (~:1507) — with the re-check gone, is the stale-category input still producible? Reason from the code (what feeds the category now) AND test: recompile the probe + the #102 shapes (`(1+2)*g` etc.) with the patch temporarily neutralized (locally, not committed) to see whether it still fires — instrument with a temporary fprintf if needed. If unreachable: replace the correction with a defensive check that ERRORS loudly instead of silently patching (comment: previously live due to the re-check; now an invariant violation), or delete it outright if the invariant makes it absurd — pick based on what the code shows, record the evidence. If still reachable: keep it, document the remaining trigger precisely.
- [ ] **Step 5: Gate** — probe passes; FULL golden 188/0 (this is the whole-suite risk task — any numeric golden regression = STOP and report, do not chase); test 76/1; ccomp-link PASS.
- [ ] **Step 6: Commit** — "refactor(types,codegen): checker records binop result type; drop mid-codegen re-check".

---

### Task 3: Stop the error cascade after a rejected declaration

**Files:**
- Modify: `src/types/type_checker.c` (`type_check_var_decl` failure paths register the variable with its declared type)
- Test: `examples/cascade_probe` is NOT possible (goldens can't assert stderr) — Makefile target `cascade-reject-probe` with an ERROR-COUNT assertion (see Step 1)
- Modify: `Makefile`

**Interfaces:** Consumes nothing from T1/T2. Standalone checker-recovery change.

- [ ] **Step 1: Probe** — `examples/cascade_reject.goo`:
```go
package main

import "fmt"

func main() {
	var b int8 = 300
	fmt.Println(int(b))
	c := b
	fmt.Println(int(c))
}
```
Makefile `cascade-reject-probe`: compile must fail rc≠0, no binary, anti-crash grep, diagnostic grep `overflows int8`, PLUS the cascade assertion: `test "$$(grep -c 'error' build/cascade_reject.err)" -le 2` (target ≤2: the real error + at most one decl-level follow-on; document the exact count achieved). Record today's count (5) in the header. Wired into `verify` (→ 24 reject probes).
- [ ] **Step 2: Verify today** — record the exact 5-error cascade (RED).
- [ ] **Step 3: Implement** — in `type_check_var_decl`'s initializer-failure paths: when the declaration carries an EXPLICIT type, register the variable(s) in scope with that type before returning the failure (mirror the success path's registration — find and reuse it, do not duplicate the TypedVar construction if a helper exists). The function still returns failure (compilation still fails; codegen never runs). For `:=`/inferred declarations where the RHS check failed: investigate whether a partial RHS type is available; if yes and cheap, register with it; if not, leave as-is and RECORD the finding + the residual cascade shape ("`:=` chains still cascade; declared-type decls don't"). Do NOT invent an error/sentinel type.
- [ ] **Step 4: Cross-checks** — the probe's `c := b` line: with `b` registered as int8, `c := b` now typechecks fine (no error) and `int(c)` resolves — total errors drop to 1 (+ whatever the `:=` residual is; record). Verify a MULTI-error program still reports each REAL error: two independent bad decls → exactly 2 errors. Verify `floatint-reject-probe` and the const* reject probes still PASS (their diagnostic greps must not be disturbed by fewer cascade lines; the anti-count assertions are only in the new probe).
- [ ] **Step 5: Gate** — full golden 188/0; 24 reject probes; test 76/1.
- [ ] **Step 6: Commit** — "fix(types): register declared-type variables on initializer failure (stop error cascade)".

---

## Final gate

`make verify` → ALL GREEN (188/0 + 24 reject probes). `make test` → 76/1. `make ccomp-link` → PASS.

## Self-review notes

- T2's Step 4 refuses to assume the category patch is dead — it demands evidence (neutralize-and-observe) before demoting it, because #102's review found exactly this class of "assumed dead, actually load-bearing" arm (the float64 ERANGE branch).
- T3 deliberately stops short of a sentinel type system — declared-type registration alone kills the observed cascade; the `:=` residual is recorded, not speculatively engineered (YAGNI).
- T1's Step 3 records which rejection path each float-% shape takes rather than assuming the new check catches all four — the #101 fix round showed these shapes route through different blocks.
- Out of scope (recorded): the `:=` cascade residual (if any); statement-level cascade sources other than var-decl (assignment, return — same pattern, separate task if the probe shows they matter).
