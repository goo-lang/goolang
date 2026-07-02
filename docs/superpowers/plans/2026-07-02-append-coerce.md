# Append Element Coercion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `append(s, e)` performs NO element width coercion: the element is stored at its SOURCE width into a DESTINATION-width slot and `goo_slice_append` copies destination-width bytes — `append([]int64{1}, int8(0 - 5))` yields 251 (raw low byte + undef upper bytes; unsigned cases work only when the stack alloca happens to be zero). Fourth member of the signedness family (#95 var-decls, #96 constant rebuilds, #97 literal elements).

**Architecture:** In the `append` arm of `codegen_generate_call_expr` (call_codegen.c:460-473): after the lvalue load, coerce `elem_val` to `elem_llvm` with the family's rule — integer→integer: SExt/ZExt by `type_is_signed(ev->goo_type)` (default signed) when narrower, Trunc when wider; integer→float: SIToFP/UIToFP by the same flag (the checker's `type_compatible` permits numeric→numeric, so `append([]float64{...}, intvar)` typechecks). ORDERING: capture the signedness BEFORE `value_info_free(ev)` — the current code frees `ev` before the slot store.

**Tech Stack:** C23, LLVM-C. One file.

## Global Constraints

- Branch: `fix/append-coerce` (already created off main @aed97e1 — do NOT commit on main).
- Commits: conventional style, imperative mood, `--no-gpg-sign`. Stage only named files; never stage anything under `.superpowers/`.
- Gate: `make lexer`, probe, then `eval "$(opam env --switch=default)"` STANDALONE, then `make verify` (all PASS; golden 172/0 → 173/0) and `make test` (76 pass / 1 pre-existing skip). `append-probe` and the slice goldens MUST stay green.
- Probes: same-width prints; float checks as bool comparisons.

## Reference: verified code landmarks (2026-07-02, main @aed97e1)

- The append arm: `src/codegen/call_codegen.c:460-481` — `elem_val` (source width, post-lvalue-load) stored into `elem_slot = codegen_create_entry_alloca(elem_llvm)`; `value_info_free(ev)` at :469 runs BEFORE the store (capture signedness first).
- The family rule reference: `slice_coerce_elem`, `src/codegen/composite_codegen.c:897+` (post-#97: `src_signed` parameter, SExt/ZExt/Trunc + SIToFP/UIToFP arms) — it is file-static; INLINE the same arms here rather than exporting it (matches how lowlevel_codegen's send path carries its own copy; a three-site DRY refactor is a separate cleanup, note it in the report).
- `type_is_signed`: `src/types/types.c`.
- Existing golden: `examples/append_probe.goo` (or the `append-probe` Makefile target) — same-width appends; must stay green.

---

### Task 1: Coerce append's element to the slice element type

**Files:**
- Modify: `src/codegen/call_codegen.c:460-473` (the append arm)
- Test: `examples/append_coerce_probe.goo` + `examples/append_coerce_probe.expected.txt`

**Interfaces:**
- Consumes: `type_is_signed`; the append arm's existing slot/call machinery (unchanged below the coercion).
- Produces: `append` narrows/widens elements correctly by source signedness, including int→float destinations.

- [ ] **Step 1: Write the failing probe**

`examples/append_coerce_probe.goo`:
```go
package main

import "fmt"

func main() {
	s := []int64{1}
	y := int8(0 - 5)
	s = append(s, y)
	fmt.Println(int(s[1]))
	u := uint8(200)
	s = append(s, u)
	fmt.Println(int(s[2]))
	w := uint16(40000)
	s = append(s, w)
	fmt.Println(int(s[3]))
	fmt.Println(int(s[0]))
	fmt.Println(len(s))
	f := []float64{1.5}
	n := int32(0 - 2)
	f = append(f, n)
	fmt.Println(f[1] == 0.0 - 2.0)
}
```

`examples/append_coerce_probe.expected.txt`:
```
-5
200
40000
1
4
true
```

Coverage: signed-negative narrow (the 251 bug), unsigned narrows at two widths (undef-upper-bytes cases), original element intact after growth, length after three appends, int→float append (bool compare). NOTE: if `append(f, n)` with an int source onto `[]float64` is REJECTED by the checker (append's own arm may be stricter than type_compatible), drop the float trio from the probe, adjust expected, still ADD the guarded SIToFP/UIToFP arms in code, and report the checker behavior.

- [ ] **Step 2: Verify it fails today**

Compile+run: expect 251 (or garbage) where -5 should be; unsigned lines may pass by stack luck — record all actuals verbatim, and also record the IR evidence (`--emit-llvm`, grep the `store i8 ... %append_elem_slot` narrow-store-into-wide-slot pattern).

- [ ] **Step 3: Fix**

In the append arm, after the existing lvalue-load block and BEFORE `value_info_free(ev)`, insert:

```c
            // Coerce the element to the slice's element type — the slot below
            // is elem_llvm-sized and goo_slice_append copies elem_llvm's size,
            // so a narrower value stored raw leaves undef upper bytes (an
            // int8 -5 appended onto []int64 read back as 251). Same source-
            // signedness rule as the var-decl, constant-rebuild, and literal-
            // element fixes (SExt/ZExt + SIToFP/UIToFP by the source type).
            {
                LLVMTypeRef from = LLVMTypeOf(elem_val);
                if (from != elem_llvm) {
                    int src_signed = ev->goo_type ? type_is_signed(ev->goo_type) : 1;
                    LLVMTypeKind fk = LLVMGetTypeKind(from), tk = LLVMGetTypeKind(elem_llvm);
                    if (fk == LLVMIntegerTypeKind && tk == LLVMIntegerTypeKind) {
                        unsigned fb = LLVMGetIntTypeWidth(from), tb = LLVMGetIntTypeWidth(elem_llvm);
                        if (fb < tb)
                            elem_val = src_signed
                                ? LLVMBuildSExt(codegen->builder, elem_val, elem_llvm, "append_sext")
                                : LLVMBuildZExt(codegen->builder, elem_val, elem_llvm, "append_zext");
                        else if (fb > tb)
                            elem_val = LLVMBuildTrunc(codegen->builder, elem_val, elem_llvm, "append_trunc");
                    } else if (fk == LLVMIntegerTypeKind &&
                               (tk == LLVMFloatTypeKind || tk == LLVMDoubleTypeKind)) {
                        elem_val = src_signed
                            ? LLVMBuildSIToFP(codegen->builder, elem_val, elem_llvm, "append_sitofp")
                            : LLVMBuildUIToFP(codegen->builder, elem_val, elem_llvm, "append_uitofp");
                    }
                }
            }
```

`value_info_free(ev)` moves to AFTER this block (it reads `ev->goo_type`). Everything below (slots, call, result load) unchanged.

- [ ] **Step 4: Rebuild and verify**

`make lexer`, run the probe → expected exactly. Re-emit IR and confirm the elem slot store is now elem_llvm-width. Re-run the existing append golden.

- [ ] **Step 5: Run the gate**

`eval "$(opam env --switch=default)"` (standalone), `make verify` → all PASS, golden 173/0. `make test` → 76/1.

- [ ] **Step 6: Commit**

```bash
git add src/codegen/call_codegen.c examples/append_coerce_probe.goo examples/append_coerce_probe.expected.txt
git commit --no-gpg-sign -m "fix(codegen): coerce append elements to the slice element type

append stored the element at its SOURCE width into a destination-width
slot; goo_slice_append then copied destination-width bytes, picking up
undef upper bytes — append([]int64{1}, int8(-5)) read back as 251.
Widen/narrow with the source type's signedness (SExt/ZExt, SIToFP/
UIToFP, Trunc) before the slot store — the fourth member of the
signedness-coercion family (var-decls, constant rebuilds, literal
elements). Three inline copies of this idiom now exist (send path,
literal elements, append) — DRY refactor noted as follow-up."
```

---

## Final gate

`make verify` → ALL GREEN (173/0). `make test` → 76/1. ccomp: opam env standalone, `make ccomp-link` → PASS.

## Self-review notes

- Single task: one coercion block + one free-ordering move.
- The float NOTE mirrors #97's (checker may or may not admit int→float appends; code arms land either way, probe adapts).
- DRY debt (three idiom copies) deliberately recorded, not refactored — matches the codebase's inline convention; a shared helper is a standalone cleanup.
