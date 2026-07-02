# Composite-Literal Constant Widening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the two composite-literal constant-rebuild sites that mis-widen signed-negative narrow constants: `[2]int64{int32(0 - 5), 3}` (array const fast path) and package-level `var s = []int64{int32(0 - 5)}` (global slice path) both produce 4294967291 instead of -5. Reviewer-probed reachable on main (found during PR #95).

**Architecture:** Both sites rebuild integer constants at the element width with unconditional `LLVMConstIntGetZExtValue` (LLVM 22 dropped const-expr casts). Zero-extraction of a signed-negative narrow constant loses the sign — `LLVMConstInt(i64, 4294967291, /*signed=*/1)` does NOT re-extend. The correct extraction signedness is the SOURCE type's (a `uint32` 4e9 must zero-extend; an `int32` -5 must sign-extend) — exactly the fix PR #95 shipped in `function_codegen.c:1037-1041` (`GetSExtValue`/`GetZExtValue` by `use_sext`). Structural wrinkle: the array site still has the element's `ValueInfo` (source `goo_type`) in scope; the slice GLOBAL path rebuilds later from a bare `LLVMValueRef` array after the ValueInfos are freed — the rebuild must MOVE into the element-collection loop (where `v->goo_type` is live), becoming a no-op for the local path (the later `slice_coerce_elem` builder coercion sees an already-width-correct constant, which it leaves alone or coerces trivially).

**Tech Stack:** C23, LLVM-C. Codegen-only, one file.

## Global Constraints

- Branch: `fix/composite-const-widen` (already created off main @7c88407 — do NOT commit on main).
- Commits: conventional style, imperative mood, `--no-gpg-sign`. Stage only named files; never stage anything under `.superpowers/`.
- Gate: `make lexer`, probe, then `eval "$(opam env --switch=default)"` STANDALONE, then `make verify` (all PASS; golden 170/0 → 171/0) and `make test` (76 pass / 1 pre-existing skip). Array/slice goldens (array/slice literal probes, global-table probes like the math/bits len8tab shapes, `conv-probe`, `int64-probe`) MUST stay green.
- Probes: same-width prints only (wrap in `int(...)` where needed).

## Reference: verified code landmarks (2026-07-02, main @7c88407)

- Array-literal const fast path: `src/codegen/composite_codegen.c:1170-1181` — has `ev` (ValueInfo with `goo_type`) in scope when rebuilding `v = LLVMConstInt(llvm_elem, LLVMConstIntGetZExtValue(v), type_is_signed(elem_type))`; `value_info_free(ev)` follows immediately.
- Slice-literal element collection loop: `src/codegen/composite_codegen.c:921-969` — `v` (ValueInfo, `v->goo_type`) live; `elem_vals[idx] = v->llvm_value; value_info_free(v);` at the end.
- Slice GLOBAL path late rebuild (the one to REPLACE/EMPTY): `src/codegen/composite_codegen.c:988-996` — loops bare `elem_vals`, no source type available.
- The correct pattern (PR #95): `src/codegen/function_codegen.c:1030-1041` — `use_sext` from source `goo_type` (`type_is_signed`, default 1 when NULL), `raw = use_sext ? (unsigned long long)LLVMConstIntGetSExtValue(...) : LLVMConstIntGetZExtValue(...)`, `LLVMConstInt(target, raw, use_sext)`.
- Local slice path width handling: `slice_coerce_elem` (same file, search it) — builder-based; verify it tolerates an already-correct-width constant (it should no-op or emit a trivial cast).

---

### Task 1: Signedness-correct constant rebuilds at both composite sites

**Files:**
- Modify: `src/codegen/composite_codegen.c` (array fast path ~L1170-1181; slice collection loop ~L921-969; slice global late-rebuild ~L988-996)
- Test: `examples/composite_widen_probe.goo` + `examples/composite_widen_probe.expected.txt`

**Interfaces:**
- Consumes: `type_is_signed`, `LLVMConstIntGetSExtValue`/`GetZExtValue` (the PR #95 pattern).
- Produces: narrow signed-negative and unsigned-large constants widen correctly in array literals (local const fast path AND keyed forms), local slice literals, and package-level global slice/array literals.

- [ ] **Step 1: Write the failing probe**

`examples/composite_widen_probe.goo`:
```go
package main

import "fmt"

var gs = []int64{int32(0 - 5), 3}
var ga = [2]int64{int32(0 - 7), 4}

func main() {
	a := [2]int64{int32(0 - 5), 3}
	fmt.Println(int(a[0]))
	fmt.Println(int(a[1]))
	s := []int64{int32(0 - 6), 2}
	fmt.Println(int(s[0]))
	fmt.Println(int(s[1]))
	u := []int64{int64(uint32(4000000000))}
	fmt.Println(u[0] == int64(4000000000))
	fmt.Println(int(gs[0]))
	fmt.Println(int(gs[1]))
	fmt.Println(int(ga[0]))
	fmt.Println(int(ga[1]))
	k := [3]int64{2: int32(0 - 9)}
	fmt.Println(int(k[2]))
	fmt.Println(int(k[0]))
}
```

`examples/composite_widen_probe.expected.txt`:
```
-5
3
-6
2
true
-5
3
-7
4
-9
0
```

Coverage: local array const fast path (the reviewer's exact shape), local slice literal, unsigned-large element (must NOT sign-extend — printed as a same-width bool comparison), global slice AND global array with negative narrows, keyed array element with zero-fill neighbor.
NOTE: if `int64(uint32(4000000000))` doesn't constant-fold and the element takes the non-constant path, that's fine — it exercises the builder coercion; keep the line. If any GLOBAL form fails to compile for a pre-existing unrelated reason (e.g. global array literals unsupported), drop that pair, adjust the expected file, and report the limitation — do NOT force it.

- [ ] **Step 2: Verify the miscompile today**

Compile+run: expect `4294967291`-style values where -5/-6/-7/-9 should be (record actuals verbatim). Some lines may accidentally pass (non-constant paths) — record which.

- [ ] **Step 3: Fix**

1. **Array fast path (~L1170-1181):** choose extraction by the SOURCE type — `ev->goo_type` is in scope:
```c
                if (is_c && ew && LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMIntegerTypeKind
                          && LLVMTypeOf(v) != llvm_elem) {
                    // Rebuild the constant at the element width (LLVM 22 dropped
                    // the LLVMConst{ZExt,IntCast} const-expr casts). Extraction
                    // must follow the SOURCE type's signedness: zero-extracting
                    // a signed-negative narrow constant loses the sign (int32 -5
                    // became 4294967291 — the signed flag on LLVMConstInt does
                    // not re-extend an already-widened raw value). Same rule as
                    // the var-decl initializer rebuild in function_codegen.c.
                    int src_signed = ev->goo_type ? type_is_signed(ev->goo_type)
                                                  : type_is_signed(elem_type);
                    unsigned long long raw = src_signed
                        ? (unsigned long long)LLVMConstIntGetSExtValue(v)
                        : LLVMConstIntGetZExtValue(v);
                    v = LLVMConstInt(llvm_elem, raw, src_signed);
                }
```
2. **Slice collection loop (~L921-969):** immediately before `elem_vals[idx] = v->llvm_value;`, add the same rebuild for constant integer values whose LLVM type differs from `llvm_elem` (guard: `LLVMIsConstant`, integer kinds both sides, `!v->is_lvalue`). Same comment, one line noting it runs here because the global path's later loop has no source type.
3. **Slice global late-rebuild (~L988-996):** the width rebuild is now redundant for integer constants (done at collection) — REPLACE the rebuild body with a comment pointing at the collection-loop normalization, keeping the `!LLVMIsConstant` error check intact. Do not leave two rebuilds (the late one would re-extract with the old wrong rule and mangle the corrected value — verify by reading what LLVMConstIntGetZExtValue of an already-i64 constant does when `LLVMTypeOf == llvm_elem`: the existing guard `ft != llvm_elem` should already skip same-width values; confirm and rely on it, or remove the late rebuild entirely).

- [ ] **Step 4: Rebuild and verify**

`make lexer`, run the probe → expected exactly. Also re-run the reviewer's shapes from the PR #95 review if present under build/review/ (composite ones), and spot-check a global byte table still works (`grep -l "len8tab\|\[256\]byte" examples/*.goo | head -2` → compile one).

- [ ] **Step 5: Run the gate**

`eval "$(opam env --switch=default)"` (standalone), `make verify` → all PASS, golden 171/0. `make test` → 76/1.

- [ ] **Step 6: Commit**

```bash
git add src/codegen/composite_codegen.c examples/composite_widen_probe.goo examples/composite_widen_probe.expected.txt
git commit --no-gpg-sign -m "fix(codegen): widen composite-literal constants by source signedness

The array and slice constant-rebuild sites zero-extracted narrow
constants unconditionally, so [2]int64{int32(-5), 3} and global
var s = []int64{int32(-5)} produced 4294967291 (the signed flag on
LLVMConstInt does not re-extend an already-widened raw value). Extract
with the SOURCE type's signedness — the var-decl rebuild's rule from
the previous fix — and normalize slice elements at collection time,
where the source type is still in scope (the global path's late
rebuild had only bare LLVMValueRefs)."
```

---

## Final gate

`make verify` → ALL GREEN (171/0). `make test` → 76/1. ccomp: opam env standalone, `make ccomp-link` → PASS.

## Self-review notes

- Single task, one file: the three touch-points are one coherent change (the collection-loop move exists only because the global path lacks source types at rebuild time).
- The keyed-array probe line guards the fast path's keyed branch (`cur` indexing) against regression from the edit.
- Unsigned coverage rides a bool print to stay same-width.
