# Design: uniform slice/array index bounds checking

**Date:** 2026-07-05
**Branch:** `fix/index-bounds-checking` (base main @ ba1d853)
**Queue item:** NEXT QUEUE #2 — "Safety: slice-index WRITE bounds checking (reads have it, writes don't — pre-existing, found #108)". Scope widened during brainstorming to also close the discovered array-read gap (user-approved: "all index bounds uniform").

## Problem

`goo_bounds_check(index, length, file, line)` panics ("bounds check failed") when `index >= length`, aborting before an out-of-range element access. It is emitted by `codegen_emit_bounds_check` (currently `static` in `src/codegen/composite_codegen.c`). Today only **one of the four** slice/array index element-address sites calls it:

| Access | Path / location | Bounds-checked today? |
|---|---|---|
| slice READ  `s[i]` | `codegen_generate_index_expr` (composite_codegen.c ~160) | ✅ yes |
| array READ  `a[i]` | `codegen_generate_index_expr` (composite_codegen.c ~110-135) | ❌ **no** |
| slice WRITE `s[i]=x` | `codegen_emit_lvalue_address` (expression_codegen.c ~826-840) | ❌ **no** |
| array WRITE `a[i]=x` | `codegen_emit_lvalue_address` (expression_codegen.c ~812-824) | ❌ **no** |

The three unchecked sites let `a[i]=x`, `arr[i]=x`, and `arr[i]` (read) access past the backing buffer / fixed array with no guard — a memory-safety hole. `a[i]++` / `a[i] += n` inherit the write gap (they resolve their target through `codegen_emit_lvalue_address`). Both paths already widen the index identically via `codegen_widen_index` (unsigned-narrow indices zero-extend, signed sign-extend), so the signedness handling is correct at every site; only the bounds-check call is missing.

## Approach

**Mirror the existing check** at each uncovered site — the chosen approach over unifying the two element-address paths.

- *Mirror (chosen):* the runtime helper and the exact call shape already exist; each uncovered site adds one call before its GEP. Lowest-risk for a safety fix; does not disturb the write path's base handling. Cost: the check-call appears at 4 sites (1 existing + 3 new) rather than 1.
- *Unify (rejected):* make `codegen_emit_lvalue_address`'s index arm reuse the read path's `codegen_generate_index_expr` (which already returns a bounds-checked lvalue). Eliminates duplication but is a structural refactor of the write path — the two paths handle their base differently (the write path needs the base as an addressable lvalue to write through), so the regression surface is larger than the bug being fixed. Not warranted here; a separate refactor if ever desired.

## Components

1. **Promote the helper to shared.** Change `codegen_emit_bounds_check` from `static` to non-static in `composite_codegen.c` (body unchanged) and add its prototype to **`include/codegen.h`** (next to `codegen_widen_index` at ~line 308): `void codegen_emit_bounds_check(CodeGenerator* codegen, LLVMValueRef index, LLVMValueRef length, ASTNode* expr);`. Both `composite_codegen.c` and `expression_codegen.c` already `#include "codegen.h"`. Single source of truth for the emit logic (index re-widening, `goo_bounds_check` call, file/line args) instead of duplicating it into the write file.

   **Build note (repo gotcha):** the Makefile has no header→object dependencies, so after editing `include/codegen.h` the build MUST be `make clean && make lexer` — a plain `make lexer` can leave stale objects that silently miscompile.

2. **Three new call sites**, each immediately before the element GEP, passing the already-computed `idx64`:
   - **Array READ** — `composite_codegen.c` `TYPE_ARRAY` arm (~110-135): before the GEP, `codegen_emit_bounds_check(codegen, idx64, LLVMConstInt(i64, base_type->data.array.length, 0), expr)`.
   - **Slice WRITE** — `expression_codegen.c` `TYPE_SLICE` arm (~826-840): after loading `slice_val`, extract the length (header field 1 — currently only field 0/data-ptr is read there), then `codegen_emit_bounds_check(codegen, idx64, slice_len, expr)` before the GEP.
   - **Array WRITE** — `expression_codegen.c` `TYPE_ARRAY` arm (~812-824): before the GEP, `codegen_emit_bounds_check(codegen, idx64, LLVMConstInt(i64, base_type->data.array.length, 0), expr)`.

   Length source per kind: **slice** = header field 1 (a loaded `size_t`); **array** = the static `base_type->data.array.length` as an `i64` constant. `codegen_emit_bounds_check` re-widens the index internally, so passing an already-`i64` `idx64` is a no-op pass-through.

## Data flow

Unchanged control flow — the check is a straight-line call, no IR branching (the `index >= length` test lives inside `goo_bounds_check`, which `abort()`s on failure). The element GEP still executes on the in-bounds path exactly as before. Because both element-address paths already funnel their index through `codegen_widen_index`, the value handed to the bounds check and the value handed to the GEP are the same `idx64`, so they agree (the property that fixed the `len8tab` unsigned-index bug on the read path).

## Error handling / semantics

Identical to the read path: on out-of-range, `goo_bounds_check` prints "bounds check failed" (with file:line) and aborts with a non-zero exit. A negative signed index sign-extends to a large `size_t` and fails `index >= length` — a bounds panic, not a wrap. This matches Go's runtime panic for out-of-range slice/array access.

## Testing

Extend the existing `bounds-probe` family in the `Makefile` (currently: slice READ `s[5]` on a len-3 slice aborts with a bounds message, and an in-bounds read exits 0). Add probes:

- **slice WRITE OOB** — `s[5] = x` on a len-3 slice aborts with the bounds message.
- **array WRITE OOB** — `arr[i] = x` with a **variable** `i` out of range aborts.
- **array READ OOB** — `arr[i]` with a **variable** `i` out of range aborts.
- **negative-index WRITE** — `s[neg] = x` aborts (sign-extend path).
- **in-bounds write/read still work** — go-run-verified: an in-bounds `s[i]=x` / `arr[i]=x` followed by reading the value back produces the correct result and exits 0, byte-identical to `go run` on the equivalent `.go`.

Each new probe target is wired into `verify:`. The full-sweep gates are unchanged: golden count grows only if any probe is added as a go-run golden (the in-bounds cases are; the abort cases are Makefile probes, not goldens); `make test` 76/1; bison **untouched (no grammar change)** — `./scripts/grammar-tripwire.sh` must stay 82/256; `make verify` ALL GREEN; `ccomp-link` PASS.

### Test-design caveat (load-bearing)

For **arrays**, a **constant** out-of-bounds index (`arr[5]` on `[3]int`) is a **compile-time error in Go** ("index 5 out of bounds [0:3]"), not a runtime panic. So every array OOB probe MUST use a **variable** index (`i := 5; arr[i] = x` / `_ = arr[i]`) to (a) be comparable against `go run` and (b) actually exercise the new *runtime* check. Slice OOB probes may use a constant index (Go bounds-checks slices at runtime regardless).

## Out of scope

- **Constant-index compile-error for fixed arrays** (`arr[5]` on `[3]int` rejected at typecheck, matching Go). This is a separate typecheck concern; this fix mirrors the read path's *runtime* approach only. Recorded as a follow-up.
- **Unifying the read/write element-address paths** (the rejected approach above).
- **Slice-expression bounds** (`s[low:high]` in `codegen_generate_slice_index_expr`, composite_codegen.c ~241 — "Bounds checking is deferred"). Separate deferred item; this fix is index (`s[i]`) bounds, not slice-expression bounds.
- **String index writes** — strings are immutable; `s[i] = x` already rejects at typecheck. No change.
