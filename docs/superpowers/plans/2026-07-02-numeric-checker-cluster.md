# Numeric Checker Cluster Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the checker-layer cluster PR #99 exposed, plus two adjacent hard failures: (T1) global-scope `var g = append(...)` SEGFAULTS the compiler; (T2) float literals never adapt their checker-stamped type (`h := g * 2.0` with g float32 stamps h FLOAT64 while codegen computes float32) and binop unification keys on `LLVMIsConstant` (so `float32(0.1) == 0.1` diverges from Go); (T3) implicit float→int assignment is a silent bit-store miscompile (`i = float32var` → 1075838976) — reject it; (T4) the user-call arg path performs NO numeric width reconciliation (`takesF64(float32val)` fails LLVM verification).

**Architecture:** T1 is a codegen guard (clean error before the unpositioned-builder crash). T2 mirrors the existing int mechanism — `is_untyped_int_rooted` / `adapt_untyped_int_operand` (expression_checker.c:536-570, call site :610-628) — for TOKEN_FLOAT literals, makes `codegen_generate_literal`'s float arm respect the stamped node_type (it hardcodes ConstReal(double) at expression_codegen.c:282-285), and switches the binop float unification predicate from LLVMIsConstant to AST-untypedness. T3 adds a float→int asymmetry to assignability (int→float stays permitted — #99's probes depend on it). T4 coerces user-call args to the declared param LLVM types via `codegen_coerce_to_type` (the #99 helper).

**Tech Stack:** C23, LLVM-C. No parser/runtime/header changes. Bison untouched (79/256).

## Global Constraints

- Branch: `fix/numeric-checker-cluster` (already created off main @62dd137 — do NOT commit on main).
- Commits: conventional, imperative, `--no-gpg-sign`. Stage only named files; never stage anything under `.superpowers/`.
- Gate per task: `make lexer`, probes, then `eval "$(opam env --switch=default)"` STANDALONE, `make verify` (all PASS; golden 177/0 grows per probe) and `make test` (76/1). **T2 and T3 change what the checker accepts/stamps — the ENTIRE golden suite is the guard. If ANY existing golden regresses, STOP and report BLOCKED with the exact rejection/mis-typing; do not loosen your change or the golden.**
- Float probe hygiene: bool-compare prints, exactly-representable values (halves), never mix print widths.

## Reference: verified code landmarks (2026-07-02, main @62dd137)

- Int adaptation mechanism (the template): `src/types/expression_checker.c:536-556` (`is_untyped_int_rooted`: TOKEN_INT literal; unary -/+/^ through; shifts through left) and `:558-...` (`adapt_untyped_int_operand`), call site `:610-628` (binop, both-int-kinds-differ branch).
- Float literal stamping: `type_check_literal`, `src/types/expression_checker.c:503-505` (unconditionally FLOAT64).
- Float literal codegen: `src/codegen/expression_codegen.c:282-285` — `LLVMConstReal(Double)` hardcoded; the INT literal arm above it reads `expr->node_type` for width (copy that pattern: if node_type is TYPE_FLOAT32 emit `LLVMConstReal(LLVMFloatTypeInContext(...), value)`).
- Binop float unification (T3 of #99): `src/codegen/expression_codegen.c:567-580` (`coerce_float_operand_widths`, predicate `LLVMIsConstant(left) != LLVMIsConstant(right)`).
- Global constant-init reject: `src/codegen/function_codegen.c:1089` ("Global variable '%s' requires constant initializer") — plain calls hit it; `append` slips past because the builtin's codegen runs BEFORE that check and crashes on the unpositioned builder (crash previously backtraced to call_codegen.c:482).
- Assignability predicate: `type_compatible`, `src/types/types.c:~638` (permits any numeric→numeric both directions).
- User-call arg loop: `src/codegen/call_codegen.c:975-1052` — has nullable auto-wrap and interface boxing per-param, NO numeric coercion; declared param types available as `param_type` in the loop.
- The helper: `codegen_coerce_to_type` (`src/codegen/codegen.c`, REQUIRES positioned builder — user calls are always in-function).
- `type_is_signed`, `type_is_integer`, `type_is_float` (verify the float predicate's exact name in types.c).

---

### Task 1: Clean error for builtin calls in global initializers

**Files:**
- Modify: `src/codegen/function_codegen.c` (global var-decl branch, before initializer codegen)
- Modify: `Makefile` (new `globalcall-reject-probe` + registration in `verify:` before `test-golden`)

- [ ] **Step 1: Verify the crash today** — `printf 'package main\nvar g = append([]int64{1}, 2)\nfunc main(){ _ = g }\n' > build/gcall.goo; bin/goo build/gcall.goo` → SIGSEGV (record exit code 139).
- [ ] **Step 2: Fix** — in `codegen_generate_var_decl`'s global path (`codegen->current_function == NULL`), BEFORE generating the initializer expression: if the initializer AST is `AST_CALL_EXPR`, emit `codegen_error(codegen, decl->pos, "Global variable '%s' requires constant initializer (calls, including builtins, run at function scope)", var_name)` and return 0. Read the surrounding code first — place the guard so plain-call behavior (already rejected at :1089 AFTER generation) is unified through the new early guard if that simplifies, or leave :1089 as the backstop; do not regress its error for non-call non-constants.
- [ ] **Step 3: Reject probe** — `globalcall-reject-probe` Makefile target (pattern: `boolnot-reject-probe`): the Step 1 source must fail cleanly (rc nonzero, NO segfault — assert rc != 139 explicitly in the probe script), no binary, grep "requires constant initializer". Register in `verify:`.
- [ ] **Step 4: Gate** — `make lexer`, probe PASS, verify all PASS (golden stays 177/0 — Makefile target, not a golden), test 76/1.
- [ ] **Step 5: Commit** — function_codegen.c + Makefile: "fix(codegen): reject builtin calls in global initializers instead of crashing".

---

### Task 2: Float literal adaptation (checker) + stamped-width emission (codegen) + AST-untypedness predicate

**Files:**
- Modify: `src/types/expression_checker.c` (`is_untyped_float_rooted` + `adapt_untyped_float_operand` + binop call site)
- Modify: `src/codegen/expression_codegen.c` (float literal arm reads node_type; `coerce_float_operand_widths` predicate)
- Test: `examples/float_adapt_probe.goo` + `.expected.txt`

**Interfaces:** Consumes the int mechanism as template. Produces: `g * 2.0` (g float32) stamps AND computes float32; `float32(0.1) == 0.1` prints true (matches Go); typed-vs-typed mixes unchanged (extend-narrower, documented Goo permissiveness).

- [ ] **Step 1: Probe**:
```go
package main

import "fmt"

func takesF32(x float32) float32 {
	return x
}

func main() {
	g := float32(2.5)
	h := g * 2.0
	fmt.Println(h == float32(5.0))
	fmt.Println(takesF32(h) == float32(5.0))
	fmt.Println(float32(0.1) == 0.1)
	q := 0.1 == float32(0.1)
	fmt.Println(q)
	d := float64(0.25)
	e := d * 2.0
	fmt.Println(e == 0.5)
	fmt.Println(g != 0.5)
	m := 2.0 * g
	fmt.Println(m == float32(5.0))
}
```
`.expected.txt`: `true` x7.
KEY LINE: `takesF32(h)` — h must now CHECK as float32 (today the checker stamps float64 and this line is a type error or verifier failure; record which). `float32(0.1) == 0.1` must be true (Go semantics — the untyped 0.1 adapts to float32; today false). Constant-on-left covered by `m` and `q`.
- [ ] **Step 2: Verify today** — record per-line actuals/type-errors verbatim.
- [ ] **Step 3: Checker** — add `is_untyped_float_rooted` (TOKEN_FLOAT literal; unary -/+ through — NO shifts/^ for floats) and `adapt_untyped_float_operand` (stamp node_type; recurse unary operand), mirroring the int pair at expression_checker.c:536+. Call site: in the binop both-floats-kinds-differ branch (add one beside the int branch at :610-628; if no such float branch exists, create it in the same shape). ALSO handle mixed literal-vs-typed where the checker currently just returns FLOAT64 from `type_check_arithmetic_op` — the adaptation must run BEFORE result-type computation so the result comes out float32.
- [ ] **Step 4: Codegen** — float literal arm (expression_codegen.c:282-285): if `expr->node_type` is float32-kind, emit `LLVMConstReal(LLVMFloatTypeInContext(codegen->context), value)`; else double (copy the INT arm's node_type-driven pattern). Then switch `coerce_float_operand_widths`'s predicate from `LLVMIsConstant` mismatch to AST-untypedness: pass the operand AST nodes in (or precompute two flags at the call site) and use `is_untyped_float_rooted` — a typed `float32(0.1)` constant no longer adapts; with the checker now stamping adapted literals correctly, the codegen unification becomes a backstop for the extend-narrower typed-mix case only. NOTE: `is_untyped_float_rooted` is checker-internal/static — either export it via a header (AVOID: header edit) or duplicate the tiny AST test locally in expression_codegen.c with a comment naming the checker original (PREFER — it is 10 lines and the pair is cross-referenced).
- [ ] **Step 5: Gate** — `make lexer`, probe passes, ALL goldens green (float_binop_probe especially — its `sum := d + g` typed-mix line must still work; if it regresses your adaptation over-fired on typed operands: STOP). Verify 178/0, test 76/1.
- [ ] **Step 6: Commit** — the two .c files + probe pair: "fix(types,codegen): untyped float literals adapt to the typed operand".

---

### Task 3: Reject implicit float→int conversions

**Files:**
- Modify: `src/types/types.c` (`type_compatible` — float→int asymmetry)
- Modify: `Makefile` (`floatint-reject-probe` + registration)

- [ ] **Step 1: Verify the miscompile today** — `g := float32(2.5); var i int64 = g` and `i = g` assignment both compile and bit-store (record values); also `append([]int64{1}, g)` (the #98 reviewer's silent case).
- [ ] **Step 2: Fix** — in `type_compatible`, where numeric→numeric passes: reject FLOAT-kind source → INT-kind target (return 0). Int→float stays permitted (PR #99 probes rely on it: `var y float64 = x`, `[]float64{1, 2.5}`). Read the function fully first — if comparisons or other non-assignment contexts route through type_compatible symmetrically (a==b checks both directions?), verify a float/int COMPARISON (e.g. `g > 1`) still works after the change — Go allows it via constant adaptation; if it breaks, scope the rejection to the assignability call sites instead (var-decl init, assignment, append elem, composite elems, channel send, call args — report which route you took and why).
- [ ] **Step 3: Reject probe** — `floatint-reject-probe`: `var i int64 = float32(2.5)` must fail cleanly, grep the diagnostic (write an error message naming the conversion: "cannot use float32 as int64 (explicit conversion required)" — match house style). Register in `verify:`.
- [ ] **Step 4: Gate** — full suite green (this REMOVES acceptance — any golden using implicit float→int will surface; STOP and report if one does). Verify 178/0 (no new golden), test 76/1.
- [ ] **Step 5: Commit** — types.c + Makefile: "fix(types): reject implicit float-to-int conversion (was a silent bit-store)".

---

### Task 4: User-call argument width coercion

**Files:**
- Modify: `src/codegen/call_codegen.c:975-1052` (the user-call arg loop)
- Test: `examples/call_arg_coerce_probe.goo` + `.expected.txt`

- [ ] **Step 1: Probe**:
```go
package main

import "fmt"

func takesF64(x float64) float64 {
	return x
}

func takesI64(n int64) int64 {
	return n
}

func takesI8(n int8) int8 {
	return n
}

func main() {
	g := float32(2.5)
	fmt.Println(takesF64(g) == 2.5)
	u := uint8(200)
	fmt.Println(int(takesI64(u)))
	y := int8(0 - 5)
	fmt.Println(int(takesI64(y)))
	big := int64(300)
	fmt.Println(int(takesI8(big)))
	fmt.Println(int(takesI64(3)))
}
```
`.expected.txt`: `true` `200` `-5` `44` `3`.
(300 → int8 truncates to 44 — Goo's checker admits the narrowing mix; document. If the checker REJECTS any line (e.g. float32→float64 param after T3, or narrowing), apply the fallback protocol: drop the line, adjust expected, report — the remaining lines still validate the coercion.)
- [ ] **Step 2: Verify today** — record per line: verifier failure (float32→float64 param) vs silent wrong (narrow ints, undef upper bytes through the arg) vs accidental pass.
- [ ] **Step 3: Fix** — in the arg loop, after each arg's ValueInfo is generated/loaded (mirror the loop's existing lvalue handling — verify whether it loads lvalues; if not, that is the same family bug: load first, report it): when `param_type` is known and numeric, `args[i] = codegen_coerce_to_type(codegen, args[i], type_is_signed(arg goo_type, default 1), codegen_type_to_llvm(codegen, param_type))`. Do not touch the nullable/interface arms.
- [ ] **Step 4: Gate** — `make lexer`, probe passes, all goldens green (call-heavy goldens: methods, ptr_recv, pkg-argcheck). Verify 179/0, test 76/1.
- [ ] **Step 5: Commit** — call_codegen.c + probe pair: "fix(codegen): coerce user-call arguments to declared parameter widths".

---

## Final gate

`make verify` → ALL GREEN (179/0). `make test` → 76/1. ccomp: opam env standalone, `make ccomp-link` → PASS.

## Self-review notes

- T2 before T4: call-arg probes assume the checker stamps `h` float32 (T2's adaptation).
- T3's type_compatible route has an explicit escape hatch (call-site scoping) if comparisons break — the implementer reports which route reality forced.
- Deliberately NOT in scope: overflowing-constant range checks (Go rejects `append([]int8{}, 130)` — needs constant-value tracking in the checker, its own task); typed f32/f64 mix rejection (documented Goo permissiveness; #99 probes depend on extend-narrower).
