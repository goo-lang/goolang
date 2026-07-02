# Cross-Kind Untyped Constants Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend Goo's untyped-constant adaptation across kinds and sinks, closing the (1v) cluster — all reproduced on main @f416e4c: `1 < g` / `g > 1` (int literal vs float32 var: verifier crash / stricter-than-Go rejection), `0.1 * 10` (untyped float × untyped int: `fmul double, i64` verifier crash; Go computes 1.0), `2.0*3.0*g` (rootedness doesn't traverse arithmetic binops; Go gives float32), and `Q{F: 2.5}` (untyped float into a float32 struct field mis-evaluates).

**Architecture:** Three tasks extending the existing lightweight adaptation mechanism (NOT full Go constant evaluation — no arbitrary-precision arithmetic; literal AST stamping + stamp-aware emission, the model PRs #92-#100 established). (T1) Cross-kind adaptation at the binop site: an untyped-INT-rooted operand meeting a FLOAT-kind operand adapts to the float type; the INT-literal codegen arm gains float-stamp emission (`LLVMConstReal((double)value)`). Fixes `1 < g`, `g > 1`, `g * 2`. (T2) Rootedness through arithmetic binops: both rooted predicates (and the codegen duplicate) gain a binop leg; adaptation recurses through binop nodes stamping children and the node itself; untyped×untyped cross-kind (`0.1*10`) resolves float-side-wins. Fixes `0.1*10`, `2.0*3.0*g`, `1 + 0.5`. (T3) The composite field-init sink: struct-literal field checking adapts untyped-numeric-rooted values to numeric field types. Fixes `Q{F: 2.5}` and `Q{F: 1}`.

**Tech Stack:** C23, LLVM-C. Checker + literal-emission codegen only. No parser/runtime/header changes. Bison untouched (79/256).

## Global Constraints

- Branch: `fix/cross-kind-constants` (already created off main @f416e4c — do NOT commit on main).
- Commits: conventional, imperative, `--no-gpg-sign`. Stage only named files; never stage anything under `.superpowers/`.
- Gate per task: `make lexer`, probes, then `eval "$(opam env --switch=default)"` STANDALONE, `make verify` (all PASS; golden 180/0 grows per probe) and `make test` (76/1). **Every task changes checker stamping — the full golden suite is the guard; STOP/BLOCKED on any regression.** Adaptation goldens especially: `float_adapt_probe`, `float_binop_probe`, `int_width_probe`, `shift_width_probe`, `var_width_probe`.
- Go conformance: where `go` is on PATH (go1.26 was used in prior reviews), derive/verify probe expectations with `go run` and record the comparison; otherwise reason from the Go spec and say so.
- Probe hygiene: bool-compare floats, exactly-representable values, same-width prints.
- CROSS-FILE INVARIANT: the checker's rooted predicates and codegen's duplicated `is_float_literal_node` (expression_codegen.c:120, change-together comments) must stay in sync — T2 extends BOTH.

## Reference: verified code landmarks (2026-07-02, main @f416e4c)

- Rooted predicates + adapters: `src/types/expression_checker.c:536+` (`is_untyped_int_rooted`, `adapt_untyped_int_operand`) and the float pair Task 2 of #100 added nearby (`is_untyped_float_rooted`, `adapt_untyped_float_operand`).
- Binop adaptation call sites: `src/types/expression_checker.c:610-628` (int-int block) and the float-float block below it (#100). The NEW cross-kind block goes beside them.
- INT literal emission: `src/codegen/expression_codegen.c:~250-280` — reads `expr->node_type` for WIDTH (narrow loop), defaults i64. The float-stamp arm goes before the width loop: if node_type kind is TYPE_FLOAT32/TYPE_FLOAT64 → `LLVMConstReal(<that type>, (double)value)`; mind signedness of the parsed value when casting (value parsed as unsigned long long? READ the arm — negative literals arrive via unary minus, so the raw value is non-negative; note this in a comment).
- FLOAT literal emission (already stamp-aware, the model): `expression_codegen.c:282-300`.
- Codegen duplicate predicate: `is_float_literal_node`, `expression_codegen.c:120` (used by `coerce_float_operand_widths`).
- Comparison checker: `type_check_comparison_op`, `src/types/expression_helpers.c:159` — plain `type_compatible` check; adaptation must happen BEFORE it (at the binop call site), not inside it.
- Struct-literal field checking: `type_check_struct_literal`, `src/types/expression_checker.c:278+`; field-value loops at `:383` and `:432` (identify keyed vs positional; the adaptation hook goes where each field value's type is checked against the declared field type).
- Shift caution: `adapt_untyped_int_operand` recurses through SHIFT nodes — a shift must NEVER be stamped to a float type (`1<<2 > g` stays rejected; Go's rules there are subtle and out of scope — document).

---

### Task 1: Cross-kind adaptation (int literal ↔ float context) + float-stamped int emission

**Files:**
- Modify: `src/types/expression_checker.c` (new cross-kind block at the binop adaptation site)
- Modify: `src/codegen/expression_codegen.c` (INT literal float-stamp arm)
- Test: `examples/cross_kind_probe.goo` + `.expected.txt`

**Interfaces:**
- Produces: an untyped-int-rooted operand meeting a FLOAT-kind operand (typed var OR stamped float literal) adapts to that float type; TOKEN_INT literals stamped float emit ConstReal. T2 builds recursion on this; T3 reuses the adapters.

- [ ] **Step 1: Probe**:
```go
package main

import "fmt"

func takesF32(x float32) float32 {
	return x
}

func main() {
	g := float32(2.5)
	if g > 1 {
		fmt.Println(1)
	}
	if 1 < g {
		fmt.Println(2)
	}
	f := g * 2
	fmt.Println(f == float32(5.0))
	fmt.Println(takesF32(f) == float32(5.0))
	d := float64(0.25)
	fmt.Println(d + 1 == 1.25)
	fmt.Println(2 * g == float32(5.0))
	if g >= 2 {
		fmt.Println(3)
	}
	fmt.Println(g != 3)
}
```
`.expected.txt`: `1` `2` `true` `true` `true` `true` `3` `true`.
Cover: comparison both orders (the crash + the stricter-than-Go rejection), arithmetic both orders, float64 context, >=/!= operators, result-type flow into a call.
- [ ] **Step 2: Verify today** — record per line: `g > 1` clean rejection (post-#100), `1 < g` verifier crash (`icmp slt i64, float`), arithmetic mixes' behavior. Where `go` is available, `go run` the probe body to confirm expectations.
- [ ] **Step 3: Checker** — add the cross-kind block beside the int-int/float-float blocks: when one operand type is float-kind and the other is int-kind AND the int side `is_untyped_int_rooted` (and is NOT shift-rooted — check the rooted path's shape or add an explicit shift exclusion; a stamped-float shift is invalid) → `adapt_untyped_int_operand(<int side>, <float side's Type>)` and update the local type var. Both operand orders. If the int side is NOT literal-rooted (an int VARIABLE), leave it — the existing rejection stands (Go agrees).
- [ ] **Step 4: Codegen** — INT literal arm: before the width-narrowing loop, if `expr->node_type` is float-kind, emit `LLVMConstReal(codegen_type_to_llvm(...) or the context float/double type, (double)value)` with `goo_type = expr->node_type`. Comment why the raw parsed value is safe to cast (negatives arrive via unary minus above the literal).
- [ ] **Step 5: Gate** — probe passes; ALL 180 goldens green (esp. the adaptation net); verify 181/0, test 76/1.
- [ ] **Step 6: Commit** — the two .c files + probe pair: "feat(types,codegen): untyped int literals adapt to float contexts".

---

### Task 2: Rootedness through arithmetic binops + untyped×untyped cross-kind

**Files:**
- Modify: `src/types/expression_checker.c` (binop legs in BOTH rooted predicates + recursion in BOTH adapters + the untyped×untyped resolution)
- Modify: `src/codegen/expression_codegen.c` (`is_float_literal_node` gains the same legs — cross-file invariant)
- Test: `examples/const_expr_probe.goo` + `.expected.txt`

**Interfaces:**
- Consumes T1 (int-literal float emission; cross-kind block). Produces: parenthesized/chained untyped constant expressions adapt as units.

- [ ] **Step 1: Probe**:
```go
package main

import "fmt"

func takesF32(x float32) float32 {
	return x
}

func main() {
	fmt.Println(0.1*10 == 1.0)
	g := float32(2.5)
	c := 2.0 * 3.0 * g
	fmt.Println(c == float32(15.0))
	fmt.Println(takesF32(c) == float32(15.0))
	e := 1 + 0.5
	fmt.Println(e == 1.5)
	var w float32 = 2.0 * 2.0
	fmt.Println(w == float32(4.0))
	h := (1 + 2) * g
	fmt.Println(h == float32(7.5))
	fmt.Println(0.5+0.25 == 0.75)
}
```
`.expected.txt`: `true` x7.
NOTE `0.1*10 == 1.0`: Go computes this EXACTLY (arbitrary-precision constant arithmetic → 1.0). Goo will compute it at float64 RUNTIME precision: 0.1 (double) × 10 = 1.0000000000000002? VERIFY with a C one-liner or `go run` an equivalent runtime computation (`x := 0.1; fmt.Println(x*10 == 1.0)` — Go prints FALSE at runtime). If double-runtime disagrees with 1.0, CHANGE the probe line to a representable computation (`0.25*4 == 1.0` — exact in binary) and record the deviation from Go's exact constant arithmetic as a documented limitation (we stamp-and-compute, we don't constant-fold in the checker). Same scrutiny for `0.5+0.25`. All other lines use representable values.
- [ ] **Step 2: Verify today** — record actuals (`0.1*10`-class = verifier crash; `2.0*3.0*g` = float64 stamping/type error at takesF32; `(1+2)*g` behavior).
- [ ] **Step 3: Checker** — extend `is_untyped_int_rooted` with: `AST_BINARY_EXPR` with operator in {+,-,*,/,%} and BOTH sides int-rooted → rooted. Extend `is_untyped_float_rooted` with: binop {+,-,*,/} and each side (float-rooted OR int-rooted), at least one float-rooted → rooted (cross-kind constant expr is float overall — Go's kind promotion). Extend BOTH adapters to recurse through those binop shapes (stamp children — an int-rooted child under a float adaptation gets the float type — and stamp the binop node itself). Then the untyped×untyped case in the binop blocks: when BOTH sides are untyped-rooted and kinds differ (0.1*10), adapt the int-rooted side to the float side's stamped type (FLOAT64 default) — the existing float-float/[T1] cross-kind blocks may already compose to handle it once rootedness recurses; VERIFY and only add what is missing.
- [ ] **Step 4: Codegen** — extend `is_float_literal_node` with the same binop legs (keep the change-together comments accurate; the checker is the source of truth). Verify `coerce_float_operand_widths` behaves with the extended predicate (an adapted whole-expression operand is emitted at the stamped width, so the backstop should rarely fire — reason and record).
- [ ] **Step 5: Gate** — probe passes; ALL goldens green; verify 182/0, test 76/1.
- [ ] **Step 6: Commit** — "feat(types,codegen): untyped constant expressions adapt as units".

---

### Task 3: Composite field-init sink

**Files:**
- Modify: `src/types/expression_checker.c` (`type_check_struct_literal` field loops :383/:432)
- Test: `examples/field_init_probe.goo` + `.expected.txt`

**Interfaces:** Consumes T1/T2 adapters. Produces: untyped numeric literals (and rooted expressions) adapt to numeric struct field types, keyed and positional.

- [ ] **Step 1: Probe**:
```go
package main

import "fmt"

type Q struct {
	F float32
	D float64
	N int32
}

func main() {
	q := Q{F: 2.5, D: 0.25, N: 7}
	fmt.Println(q.F == float32(2.5))
	fmt.Println(q.D == 0.25)
	fmt.Println(int(q.N))
	r := Q{1.5, 1, 3}
	fmt.Println(r.F == float32(1.5))
	fmt.Println(r.D == 1.0)
	fmt.Println(int(r.N))
	s := Q{F: 1 + 0.5, D: 2 * 0.25, N: 2 + 3}
	fmt.Println(s.F == float32(1.5))
	fmt.Println(s.D == 0.5)
	fmt.Println(int(s.N))
}
```
`.expected.txt`: `true` `true` `7` `true` `true` `3` `true` `true` `5`.
Covers: keyed + positional, float literal into float32 (the reported bug) and float64, INT literal into float64 field (`D: 1` — cross-kind at the sink), rooted expressions into fields (T2 composition), int fields unaffected.
- [ ] **Step 2: Verify today** — record per line (`F: 2.5` mis-eval reproduced; which lines crash vs mis-evaluate vs work).
- [ ] **Step 3: Fix** — in both field loops, after resolving the declared field type and BEFORE the compat check: if the field type is numeric and the value node is untyped-rooted of a compatible kind (int-rooted → any numeric field; float-rooted → float fields only, never int fields — float→int stays rejected per #100), call the matching adapter with the FIELD type; re-run/obtain the value's type after adaptation for the compat check. Mirror the binop blocks' style. Check whether array/slice literal element checking (`check_slice_elements` :344) has the same gap for `[]float32{2.5}`-shaped LOCAL literals — the codegen const-rebuild handles constants (#99), so likely fine at runtime; verify one shape and record (fix only if broken, as a separate probe line).
- [ ] **Step 4: Gate** — probe passes; ALL goldens green (struct/composite goldens especially); verify 183/0, test 76/1.
- [ ] **Step 5: Commit** — "fix(types): untyped numeric literals adapt to struct field types".

---

## Final gate

`make verify` → ALL GREEN (183/0). `make test` → 76/1. ccomp: opam env standalone, `make ccomp-link` → PASS.

## Self-review notes

- The stamp-and-compute model (no checker-side constant folding) is a documented deviation from Go's exact constant arithmetic — T2's probe NOTE forces the implementer to confront the one case where it's observable (`0.1*10`) and choose representable values, recording the limitation honestly.
- Shift-rooted operands are explicitly excluded from float adaptation (T1 Step 3) — Go's shift-in-float-context rules are subtle and deferred.
- T3 keeps #100's float→int rejection intact at the sink (float-rooted values never adapt to int fields).
- Out of scope: overflowing-constant range checks (1o); var-decl/return/chan sinks (already handled by value coercion at codegen); full constant folding.
