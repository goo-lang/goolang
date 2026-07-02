# Element-Coercion Signedness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `slice_coerce_elem` — the builder-path element coercion for slice AND array literals — widens with unconditional `SExt` (and converts int→float with signed-only `SIToFP`), so a NON-constant unsigned element mis-widens: `x := uint8(200); s := []int64{x}` prints -56. Fix: signedness-aware coercion by SOURCE type, completing the family (var-decls PR #95, constants PR #96).

**Architecture:** Add a `src_signed` parameter to `slice_coerce_elem` (composite_codegen.c:897-911). The array-literal caller (L1261) has the element's `ValueInfo` live — pass `type_is_signed(ev->goo_type)`. The slice caller (L1065) coerces from the bare `elem_vals` array AFTER the source ValueInfos are freed — capture a parallel `int* elem_signed` array in the collection loop (same lifetime pattern PR #96 used for the constant rebuild, just as a flag instead). Widen: SExt/ZExt by flag; int→float: SIToFP/UIToFP by flag; Trunc unchanged (sign-agnostic).

**Tech Stack:** C23, LLVM-C. One file.

## Global Constraints

- Branch: `fix/elem-coerce-signedness` (already created off main @e7c75ba — do NOT commit on main).
- Commits: conventional style, imperative mood, `--no-gpg-sign`. Stage only named files; never stage anything under `.superpowers/`.
- Gate: `make lexer`, probe, then `eval "$(opam env --switch=default)"` STANDALONE, then `make verify` (all PASS; golden 171/0 → 172/0) and `make test` (76 pass / 1 pre-existing skip). Slice/array goldens incl. `composite_widen_probe` (PR #96's) MUST stay green.
- Probes: same-width prints; float checks ride bool comparisons.

## Reference: verified code landmarks (2026-07-02, main @e7c75ba)

- `slice_coerce_elem`: `src/codegen/composite_codegen.c:897-911` — `fb < tb → SExt` unconditional; `SIToFP` unconditional; `fb > tb → Trunc` (fine).
- Callers: slice build at `:1065` (`elem_vals[i]`, source types gone); array-literal alloca loop at `:1254-1265` (`ev` live, freed at `:1265`).
- Signedness rule + default: `type_is_signed(goo_type)`, default signed(1) when goo_type NULL — the convention from function_codegen.c:1030-1041 (PR #95) and the PR #96 collection-loop rebuild.
- The collection loop that must capture flags: `:955-1010` region (post-#96 it already rebuilds constants there; the flag capture rides the same loop).
- Elem allocation pattern: `elem_vals = count ? calloc(count, sizeof(LLVMValueRef)) : NULL;` — mirror for the flags array; free wherever `elem_vals` frees (grep every `free(elem_vals)` — there are several early-error frees; the flag array must free at each).

---

### Task 1: Signedness parameter for `slice_coerce_elem` + flag capture

**Files:**
- Modify: `src/codegen/composite_codegen.c` (`slice_coerce_elem` + both callers + collection-loop flag array)
- Test: `examples/elem_coerce_probe.goo` + `examples/elem_coerce_probe.expected.txt`

**Interfaces:**
- Consumes: `type_is_signed` convention (signed default on NULL type).
- Produces: non-constant narrow elements widen by source signedness in slice and array literals; int→float element conversion respects unsignedness.

- [ ] **Step 1: Write the failing probe**

`examples/elem_coerce_probe.goo`:
```go
package main

import "fmt"

func main() {
	x := uint8(200)
	s := []int64{x, 3}
	fmt.Println(int(s[0]))
	fmt.Println(int(s[1]))
	a := [2]int64{x, 4}
	fmt.Println(int(a[0]))
	fmt.Println(int(a[1]))
	y := int8(0 - 5)
	t := []int64{y}
	fmt.Println(int(t[0]))
	b := [1]int64{y}
	fmt.Println(int(b[0]))
	u := uint16(40000)
	w := []int64{u}
	fmt.Println(int(w[0]))
	f := []float64{x}
	fmt.Println(f[0] == 200.0)
}
```

`examples/elem_coerce_probe.expected.txt`:
```
200
3
200
4
-5
-5
40000
true
```

Coverage: non-constant uint8 into slice AND array (the -56 bug both ways), signed-negative non-constant control (SExt must remain), uint16 (a second unsigned width), unsigned int→float (UIToFP — bool-compare print). NOTE: if `[]float64{x}` is rejected by the type checker (int-to-float element assignability may not be permitted), drop the float pair from the probe, adjust expected, still ADD the UIToFP arm in code (guarded identically to SIToFP), and report the checker limitation.

- [ ] **Step 2: Verify it fails today**

Compile+run: expect -56 where 200 should be (slice AND array lines), 40000 possibly correct-by-luck or wrong (record actual). Record all actuals verbatim.

- [ ] **Step 3: Fix**

1. `slice_coerce_elem` signature → `static LLVMValueRef slice_coerce_elem(CodeGenerator* codegen, LLVMValueRef v, LLVMTypeRef to, int src_signed)`. Widen arm: `src_signed ? SExt : ZExt` (names "elem_sext"/"elem_zext"); float arm: `src_signed ? SIToFP : UIToFP` ("elem_sitofp"/"elem_uitofp"); Trunc unchanged. Comment: source-signedness rule, family reference (var-decl + constant-rebuild fixes).
2. Array caller (`:1261`): `slice_coerce_elem(codegen, v, llvm_elem, ev->goo_type ? type_is_signed(ev->goo_type) : 1)`.
3. Slice path: in `codegen_build_slice_from_elems`, allocate `int* elem_signed = count ? calloc(count, sizeof(int)) : NULL;` beside `elem_vals`; in the collection loop set `elem_signed[idx] = v->goo_type ? type_is_signed(v->goo_type) : 1;` (BEFORE `value_info_free(v)`); at the `:1065` call pass `elem_signed[i]`; add `free(elem_signed)` at EVERY existing `free(elem_vals)` site (grep them all — missing one is a leak, doubling one is a crash).

- [ ] **Step 4: Rebuild and verify**

`make lexer`, run the probe → expected exactly. Also re-run `composite_widen_probe` (PR #96's) directly — constant paths must be unaffected.

- [ ] **Step 5: Run the gate**

`eval "$(opam env --switch=default)"` (standalone), `make verify` → all PASS, golden 172/0. `make test` → 76/1.

- [ ] **Step 6: Commit**

```bash
git add src/codegen/composite_codegen.c examples/elem_coerce_probe.goo examples/elem_coerce_probe.expected.txt
git commit --no-gpg-sign -m "fix(codegen): coerce literal elements by source signedness

slice_coerce_elem widened every narrow element with SExt (and int-to-
float with SIToFP), so a non-constant unsigned element mis-widened:
x := uint8(200); []int64{x} produced -56. Thread the source type's
signedness through — the array caller has the element ValueInfo live;
the slice path captures a per-element flag at collection time (the
source types are freed before the coercion loop runs). Completes the
signedness family: var-decl initializers, constant rebuilds, and now
the builder element path."
```

---

## Final gate

`make verify` → ALL GREEN (172/0). `make test` → 76/1. ccomp: opam env standalone, `make ccomp-link` → PASS.

## Self-review notes

- Single task: signature change + two callers + flag plumbing are inseparable (the signature change forces every caller to choose).
- The flag-array free-site audit is called out explicitly because the collection loop has multiple early-error frees.
- The float arm rides along because it is the same unconditional-signed assumption in the same function being edited.
