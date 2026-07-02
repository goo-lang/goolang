# Float Coercion Family Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Float width coercion is missing everywhere the int family was just fixed (#95-#98). Reproduced on main @9a510f3, ALL broken: `var g float32 = 2.5` (untyped literal is a double — 8-byte store into 4-byte alloca, the #95 smash with floats), `var d float64 = f32var`, `[]float64{f32var}` literal elements, `append([]float32, f64var)` (OOB) and `append([]float64, f32var)` (undef bytes), `chan float64 <- f32var`, and `g == 2.5` fails LLVM verification (`fcmp oeq float, double`).

**Architecture:** Three tasks. (1) Introduce the shared coercion helper the int-family PRs deferred — `codegen_coerce_to_type(codegen, value, src_signed, to)` covering int↔int (SExt/ZExt/Trunc by source signedness), int→float (SI/UIToFP), float↔float (FPExt/FPTrunc) — and convert the append site to it (float probes there). With SIX sites needing identical float arms, inline copies stop being the codebase convention and start being the bug vector; the helper is the Ousterhout-deep fix. (2) Convert/fix the store-family sites: var-decl init (local path via helper; global constant path needs float constant rebuilds via `LLVMConstRealGetDouble`/`LLVMConstReal`), `slice_coerce_elem` (delegate to helper), the collection-loop constant rebuild (float constants), channel send (plain + select copy, via helper). (3) Binary-op float width unification: FPExt the narrower operand before the op switch (one prep site serves every FCmp/FAdd/FMul...).

**Tech Stack:** C23, LLVM-C. No parser/runtime changes. ONE header edit (the helper's declaration) — `make clean` required after it.

## Global Constraints

- Branch: `fix/float-coerce-family` (already created off main @9a510f3 — do NOT commit on main).
- Commits: conventional style, imperative mood, `--no-gpg-sign`. Stage only named files; never stage anything under `.superpowers/`.
- **Task 1 adds a declaration to `include/codegen.h` — run `make clean && make lexer` after any header edit** (no header deps in the Makefile; stale objects silently miscompile). Tasks 2-3 are .c-only (plain `make lexer`).
- Gate per task: probes, then `eval "$(opam env --switch=default)"` STANDALONE, `make verify` (all PASS; golden grows per probe: 174/175/176) and `make test` (76 pass / 1 pre-existing skip). The int-family probes (`var_width_probe`, `composite_widen_probe`, `elem_coerce_probe`, `append_coerce_probe`, `chan_lvalue_send_probe`) are the regression net — they MUST stay green at every task.
- Float assertions in probes ride bool comparisons (`f[0] == 2.5`) printed alongside ints — never print raw floats next to ints (mixed-width Println hazard) and never compare floats produced by arithmetic for exactness unless the values are exactly representable (use halves: 1.5, 2.5, 0.25).
- Bison count untouched: 79 S/R + 256 R/R.

## Reference: verified code landmarks (2026-07-02, main @9a510f3)

- Helper home: `src/codegen/codegen.c` (near `codegen_map_value_to_slot` ~L475); declaration in `include/codegen.h` (near `codegen_widen_index`, L228).
- The signedness-rule reference implementations to fold into the helper: append arm `src/codegen/call_codegen.c:475-495` (post-#98, all int arms already right); `slice_coerce_elem` `src/codegen/composite_codegen.c:897-915` (post-#97, `src_signed` param).
- Var-decl init block: `src/codegen/function_codegen.c:~1000-1045` (post-#95: int Trunc/SExt/ZExt local + global constant rebuild) — needs float arms in BOTH paths; global float rebuild uses `LLVMConstRealGetDouble(v, &loses)` + `LLVMConstReal(llvm_type, d)`.
- Collection-loop constant rebuild: `src/codegen/composite_codegen.c:~983-1000` (post-#96/#97, int-only guard) — float constants fall through today; a global `var s = []float32{1.5}` puts a double constant into a float array initializer (verify what actually happens — record it — then fix with ConstReal rebuild).
- Channel send coercion: `src/codegen/lowlevel_codegen.c:44-96` (int-only widen; the alloca is elem-typed so floats currently store mismatched) and the select-copy in `src/codegen/statement_codegen.c` (post-#93 mirror) — both convert to the helper.
- Binary-op operand prep: `src/codegen/expression_codegen.c:966-995` (the auto-load block) — the float unification belongs right after both operands are loaded, before the operator switch; FCmp sites at :1153+ show the symptom (`fcmp oeq float, double`).
- Float types in Goo: float32/float64 only — FPExt/FPTrunc between exactly two widths.

---

### Task 1: Shared coercion helper + append converted

**Files:**
- Modify: `include/codegen.h` (one declaration), `src/codegen/codegen.c` (the helper), `src/codegen/call_codegen.c` (append arm: replace the inline block with a helper call)
- Test: `examples/float_append_probe.goo` + `.expected.txt`

**Interfaces:**
- Produces: `LLVMValueRef codegen_coerce_to_type(CodeGenerator* codegen, LLVMValueRef v, int src_signed, LLVMTypeRef to)` — returns `v` unchanged when types already match or no arm applies; REQUIRES a positioned builder (document: local/function paths only; constant/global paths keep their ConstInt/ConstReal rebuilds). Tasks 2-3 consume it.

- [ ] **Step 1: Write the failing probe**

`examples/float_append_probe.goo`:
```go
package main

import "fmt"

func main() {
	s := []float32{1.5}
	w := float64(2.5)
	s = append(s, w)
	fmt.Println(s[1] == float32(2.5))
	fmt.Println(s[0] == float32(1.5))
	t := []float64{0.25}
	n := float32(3.5)
	t = append(t, n)
	fmt.Println(t[1] == 3.5)
	fmt.Println(t[0] == 0.25)
	fmt.Println(len(s) + len(t))
}
```

`.expected.txt`: `true` `true` `true` `true` `4` (one per line).

- [ ] **Step 2: Verify it fails today** — compile+run; record actuals (expect false lines; possibly garbage/OOB effects). Record the IR store-width evidence for the f64→f32 case (8-byte store into 4-byte slot).

- [ ] **Step 3: Implement the helper** in `src/codegen/codegen.c`:

```c
// Coerce a VALUE to the target LLVM type using the source type's
// signedness — the single home for the width-coercion rule that was
// previously inlined (and repeatedly re-broken) at the var-decl,
// literal-element, append, and channel-send sites:
//   int -> int      : SExt/ZExt by src_signed when widening, Trunc when narrowing
//   int -> float    : SIToFP/UIToFP by src_signed
//   float -> float  : FPExt widening, FPTrunc narrowing
// Anything else (matching types, aggregates, pointers) returns v unchanged.
// REQUIRES a positioned builder — callers on constant/global paths must keep
// their LLVMConstInt/LLVMConstReal rebuilds instead.
LLVMValueRef codegen_coerce_to_type(CodeGenerator* codegen, LLVMValueRef v,
                                    int src_signed, LLVMTypeRef to) {
    LLVMTypeRef from = LLVMTypeOf(v);
    if (from == to) return v;
    LLVMTypeKind fk = LLVMGetTypeKind(from), tk = LLVMGetTypeKind(to);
    int f_is_fp = (fk == LLVMFloatTypeKind || fk == LLVMDoubleTypeKind);
    int t_is_fp = (tk == LLVMFloatTypeKind || tk == LLVMDoubleTypeKind);
    if (fk == LLVMIntegerTypeKind && tk == LLVMIntegerTypeKind) {
        unsigned fb = LLVMGetIntTypeWidth(from), tb = LLVMGetIntTypeWidth(to);
        if (fb < tb) return src_signed
            ? LLVMBuildSExt(codegen->builder, v, to, "coerce_sext")
            : LLVMBuildZExt(codegen->builder, v, to, "coerce_zext");
        if (fb > tb) return LLVMBuildTrunc(codegen->builder, v, to, "coerce_trunc");
        return v;
    }
    if (fk == LLVMIntegerTypeKind && t_is_fp) return src_signed
        ? LLVMBuildSIToFP(codegen->builder, v, to, "coerce_sitofp")
        : LLVMBuildUIToFP(codegen->builder, v, to, "coerce_uitofp");
    if (f_is_fp && t_is_fp) {
        // float32<->float64 only; kind order: Float < Double.
        if (fk == LLVMFloatTypeKind && tk == LLVMDoubleTypeKind)
            return LLVMBuildFPExt(codegen->builder, v, to, "coerce_fpext");
        if (fk == LLVMDoubleTypeKind && tk == LLVMFloatTypeKind)
            return LLVMBuildFPTrunc(codegen->builder, v, to, "coerce_fptrunc");
    }
    return v;
}
```

Declaration in `include/codegen.h` next to `codegen_widen_index`. Then in the append arm (call_codegen.c:475-495) replace the whole inline coercion block with:
```c
            int append_src_signed = ev->goo_type ? type_is_signed(ev->goo_type) : 1;
            elem_val = codegen_coerce_to_type(codegen, elem_val, append_src_signed, elem_llvm);
```
(keep the free-ordering: `value_info_free(ev)` after this).

- [ ] **Step 4: `make clean && make lexer`** (header changed), probe passes; `append_coerce_probe` (int family) still green.
- [ ] **Step 5: Gate** — verify all PASS golden 174/0; test 76/1.
- [ ] **Step 6: Commit** — `git add include/codegen.h src/codegen/codegen.c src/codegen/call_codegen.c examples/float_append_probe.goo examples/float_append_probe.expected.txt`; message: "feat(codegen): shared width-coercion helper; float arms for append" with a body noting the six-site family motivation and the builder requirement.

---

### Task 2: Float arms at the store-family sites

**Files:**
- Modify: `src/codegen/function_codegen.c` (var-decl init: local via helper, global const float rebuild), `src/codegen/composite_codegen.c` (`slice_coerce_elem` delegates to helper; collection-loop const rebuild gains float constants), `src/codegen/lowlevel_codegen.c` + `src/codegen/statement_codegen.c` (both send sites via helper)
- Test: `examples/float_width_probe.goo` + `.expected.txt`

**Interfaces:** Consumes Task 1's helper. Produces: the six reproduced store-shapes correct.

- [ ] **Step 1: Probe** (`float_width_probe.goo`):
```go
package main

import "fmt"

var gf float32 = 2.5
var gs = []float32{1.5, 0.25}

func main() {
	var g float32 = 2.5
	fmt.Println(g == float32(2.5))
	fw := float32(1.5)
	var d float64 = fw
	fmt.Println(d == 1.5)
	a := []float64{fw, 0.25}
	fmt.Println(a[0] == 1.5)
	fmt.Println(a[1] == 0.25)
	b := []float32{fw}
	fmt.Println(b[0] == float32(1.5))
	ch := make_chan(float64, 1)
	ch <- fw
	r := <-ch
	fmt.Println(r == 1.5)
	c2 := make_chan(float64, 1)
	sel := 0
	select {
	case c2 <- fw:
		sel = 1
	default:
		sel = 2
	}
	fmt.Println(sel)
	r2 := <-c2
	fmt.Println(r2 == 1.5)
	fmt.Println(gf == float32(2.5))
	fmt.Println(gs[0] == float32(1.5))
	fmt.Println(gs[1] == float32(0.25))
}
```
`.expected.txt`: `true true true true true true 1 true true true true` (one per line).
NOTE: this probe USES `g == float32(2.5)` (explicit conversions both sides) so it does not depend on Task 3's binop fix. If any GLOBAL form fails to compile for a pre-existing unrelated reason, apply the established fallback protocol (drop the pair, adjust expected, report).

- [ ] **Step 2: Verify failures today** — record actuals per line AND the global-path behavior for `var gf float32 = 2.5` / `var gs = []float32{...}` (may be a verifier error or silent garbage — record which; this is the evidence for the ConstReal rebuild).
- [ ] **Step 3: Fix all sites**: var-decl local block → helper call (delete the inline int arms; pass `type_is_signed(init source goo_type)` default 1); var-decl GLOBAL constant path → add float branch (`LLVMConstRealGetDouble` + `LLVMConstReal(llvm_type, d)`, guarded `LLVMIsConstant` + FP kinds); `slice_coerce_elem` body → single `return codegen_coerce_to_type(codegen, v, src_signed, to);` (keep the static wrapper signature — callers unchanged); collection-loop const rebuild → add the float-constant arm beside the int one; both send sites → replace inline int widen blocks with helper calls (source signedness from `value_val->goo_type` as today).
- [ ] **Step 4: `make lexer`** (no header change), probe passes, ALL int-family probes green (`var_width_probe`, `composite_widen_probe`, `elem_coerce_probe`, `chan_lvalue_send_probe`).
- [ ] **Step 5: Gate** — verify 175/0; test 76/1.
- [ ] **Step 6: Commit** — the five .c files + probe pair; message: "fix(codegen): float width coercion at var-decl, literal-element, and send sites".

---

### Task 3: Binary-op float width unification

**Files:**
- Modify: `src/codegen/expression_codegen.c` (operand prep in `codegen_generate_binary_expr`, after both auto-loads ~L966-995)
- Modify: `Makefile` — NOTHING (no reject probe; this is a wrong-code/verifier fix)
- Test: `examples/float_binop_probe.goo` + `.expected.txt`

- [ ] **Step 1: Probe**:
```go
package main

import "fmt"

func main() {
	g := float32(2.5)
	if g == 2.5 {
		fmt.Println(1)
	} else {
		fmt.Println(0)
	}
	h := g * 2.0
	fmt.Println(h == float32(5.0))
	d := float64(0.25)
	if g > d {
		fmt.Println(2)
	}
	sum := d + g
	fmt.Println(sum == 2.75)
	fmt.Println(g != 0.5)
}
```
`.expected.txt`: `1` `true` `2` `true` `true`.
NOTE: `g * 2.0` — decide the RESULT type by Go rules: an untyped constant adapts to the typed operand, so the result is float32; `d + g` mixes two TYPED floats, which real Go REJECTS — check what Goo's checker does with it (it may permit via type_compatible). If the checker rejects `d + g`, drop that pair from the probe, adjust expected, and report — do NOT loosen the checker.

- [ ] **Step 2: Verify today** — `g == 2.5` is the reproduced verifier failure (`fcmp oeq float, double`); record the others' actuals.
- [ ] **Step 3: Fix** — in the binary-op operand prep (after both operands are loaded), unify float widths: if both operand LLVM types are FP kinds and differ, FPExt the NARROWER operand to the wider type via `codegen_coerce_to_type` (src_signed irrelevant for FP→FP; pass 1). THEN the untyped-constant case: a `double` constant against a `float` typed operand — Go semantics says the CONSTANT adapts to the typed operand: if one side is `LLVMIsConstant` FP and the other is a non-constant `float`, FPTrunc/rebuild the constant to the variable's type instead of extending the variable (otherwise `g == 2.5` compares at double precision — usually fine for representable halves but wrong for values that differ when rounded to float32; and `g * 2.0` must yield float32, not double). Implement: constant-vs-typed → coerce the CONSTANT to the typed side; typed-vs-typed differing widths → extend the narrower (documenting that real Go rejects this; Goo's checker currently admits it). Ensure the RESULT ValueInfo's goo_type matches what the type checker stamped (read what the checker returns for these mixes — `type_check_binary_expr` — and make codegen agree; report any checker/codegen disagreement rather than papering over it).
- [ ] **Step 4: `make lexer`**, probe passes; float goldens from Tasks 1-2 green.
- [ ] **Step 5: Gate** — verify 176/0; test 76/1.
- [ ] **Step 6: Commit** — expression_codegen.c + probe pair; message: "fix(codegen): unify float operand widths in binary ops".

---

## Final gate (after all tasks)

`make verify` → ALL GREEN (176/0). `make test` → 76/1. ccomp: opam env standalone, `make ccomp-link` → PASS.

## Self-review notes

- The helper-first order means Tasks 2-3 are conversions, not re-inventions; the header edit is isolated to Task 1 with the make-clean constraint attached.
- Task 3 carries the only real semantic judgment (constant-adapts-to-typed-operand vs extend-both); the plan states the Go rule and requires reporting checker/codegen disagreements instead of silently choosing.
- Deliberately NOT in scope: checker-side constant range/type enforcement (1o — Go rejects mixed typed floats and overflowing constants; Goo's checker looseness is a separate, checker-layer task); float→int implicit conversions (checker should reject; helper leaves them unchanged by design).
