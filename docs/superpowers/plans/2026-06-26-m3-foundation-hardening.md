# M3 Foundation Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finish what M2 started — fix int64 literal coercion (a correctness defect), add comma-ok map reads (`v, ok := m[k]`), and lower `match` guards (`case X{..} if cond:`).

**Architecture:** Three independent features, one PR each, sequenced. Goo is "the C++ to Go's C" — a compatible superset that adds power on top of a Go-faithful core; M3 hardens that core. Each feature is delivered TDD-first via an `examples/<feat>_probe.goo` gate (compile → run → diff against `.expected.txt`), wired into `make verify` and CI.

**Tech Stack:** C23 compiler; real LLVM C API codegen; yacc/bison parser (`parser.y`); Make build; runtime in C (`src/runtime/`).

## Global Constraints

- Spec: `docs/superpowers/specs/2026-06-26-m3-foundation-hardening-design.md` — every task's requirements implicitly include it.
- Build: `make goo` builds the compiler (`$(COMPILER)`); `make lib/libgoo_runtime.a` builds the runtime (`$(RUNTIME_LIB)`). **After editing any header** (`include/runtime.h`, `include/types.h`, `include/codegen.h`, `include/ast.h`): `make clean && make goo` — incremental builds miss header deps.
- A probe target follows the exact `map-probe` shape (`Makefile:416-425`): depends on `$(COMPILER) $(RUNTIME_LIB)`, compiles `examples/X.goo` to `build/X`, runs it, diffs stdout against `examples/X.expected.txt`, prints `X: PASS`/`FAIL`.
- Every new probe is added to the `verify:` aggregate (`Makefile:522`) **and** the CI `Language probes` list (`.github/workflows/tests.yml:54`).
- `make verify` failing only at the CompCert `ccomp-build` step is the known env gap — ignore it; all probe targets must pass.
- Conventional commits; imperative mood. Co-author trailer: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
- Repo: `origin` (darragh-downey/goolang) redirects to dd0wney/goolang; `gh` account `dd0wney`; same-repo merge-commit PRs. Confirm CI green by reading the run rollup, never a piped exit code.
- One feature branch per phase, off `main`. Do not commit features to `main` directly.

---

## File Structure

| File | Responsibility | Phases |
|---|---|---|
| `src/codegen/expression_codegen.c` | Binary/comparison codegen; new `int_widen_or_trunc` + `coerce_int_literal_operand` static helpers | 1 |
| `include/runtime.h`, `src/runtime/runtime.c` | New `goo_map_get_sv_ok` presence-returning getter | 2 |
| `src/types/type_checker.c` | Map-index-as-2-LHS-RHS → `TYPE_STRUCT{V,bool}` | 2 |
| `src/codegen/function_codegen.c` | 2-LHS decl over a map index → lower via `goo_map_get_sv_ok` aggregate | 2 |
| `src/codegen/composite_codegen.c` | `codegen_generate_match`: evaluate guard after payload binding, branch to fallback on false | 3 |
| `examples/int64_probe.{goo,expected.txt}` | Phase 1 gate | 1 |
| `examples/commaok_probe.{goo,expected.txt}` | Phase 2 gate | 2 |
| `examples/guard_probe.{goo,expected.txt}` | Phase 3 gate | 3 |
| `Makefile`, `.github/workflows/tests.yml` | Probe targets + CI wiring | 1,2,3 |

---

## Phase 1 — int64 / int literal coercion

Branch: `feat/m3-int64-coercion`.

### Task 1: Literal-operand coercion in binary/comparison codegen

**Files:**
- Create: `examples/int64_probe.goo`, `examples/int64_probe.expected.txt`
- Modify: `src/codegen/expression_codegen.c` (add 2 static helpers above `codegen_generate_binary_expr`; one call after operand load, ~line 515)
- Modify: `Makefile` (new `int64-probe` target; add to `verify:` line 522)
- Modify: `.github/workflows/tests.yml:54` (add `int64-probe`)

**Interfaces:**
- Consumes: `binary->left`, `binary->right` (`BinaryExprNode`, `include/ast.h:420`); `ValueInfo{llvm_value, goo_type, is_lvalue}` (`include/codegen.h:128`); `type_is_integer()` (already used in this file); LLVM C API.
- Produces: nothing consumed by later phases (self-contained).

- [ ] **Step 1: Write the failing probe**

Create `examples/int64_probe.goo`:

```goo
// int64_probe: integer literals must coerce to the other operand's width so
// int64 values compare and compute without an i64-vs-i32 module-verify
// failure. Closes the gap noted in examples/map_probe.goo.
package main

import "fmt"

func main() {
    var d int64 = -1
    if d == -1 {
        fmt.Println("PASS: int64 var compares to int literal")
    }

    var n int64 = 1000000
    if n + 1 == 1000001 {
        fmt.Println("PASS: int64 arithmetic with int literal")
    }

    deltas := map[string]int64{}
    deltas["d"] = -1
    if deltas["d"] == -1 {
        fmt.Println("PASS: int64 map value compares to int literal")
    }
}
```

Create `examples/int64_probe.expected.txt`:

```
PASS: int64 var compares to int literal
PASS: int64 arithmetic with int literal
PASS: int64 map value compares to int literal
```

- [ ] **Step 2: Wire the probe into the build**

In `Makefile`, after the `map-probe:` recipe (~line 425), add:

```makefile
int64-probe: $(COMPILER) $(RUNTIME_LIB)
	@echo "=== int64-probe: int literal coercion to int64 ==="
	$(COMPILER) -o build/int64_probe examples/int64_probe.goo
	@./build/int64_probe > build/int64_probe.actual.txt
	@if diff -u examples/int64_probe.expected.txt build/int64_probe.actual.txt; then \
	  echo "int64-probe: PASS"; \
	else \
	  echo "int64-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi
```

Append ` int64-probe` to the end of the `verify:` dependency list (`Makefile:522`).
In `.github/workflows/tests.yml:54`, append ` int64-probe` to the probe list.

- [ ] **Step 3: Build and run the probe — verify it FAILS**

Run:
```bash
make goo && make lib/libgoo_runtime.a && make int64-probe
```
Expected: FAIL — the compile aborts or the produced module fails LLVM verification with an `icmp`/`add` type mismatch (`i64` vs `i32`). This confirms the defect.

- [ ] **Step 4: Add the coercion helpers**

In `src/codegen/expression_codegen.c`, immediately above `codegen_generate_binary_expr` (line 389), add:

```c
// Widen (sign-extend) or truncate an integer value to a target LLVM type.
// Signed: Goo int literals are signed, so widening sign-extends.
static LLVMValueRef int_widen_or_trunc(CodeGenerator* codegen, LLVMValueRef v,
                                       LLVMTypeRef to_ty,
                                       unsigned from_bits, unsigned to_bits) {
    if (from_bits < to_bits)
        return LLVMBuildSExt(codegen->builder, v, to_ty, "litsext");
    if (from_bits > to_bits)
        return LLVMBuildTrunc(codegen->builder, v, to_ty, "littrunc");
    return v;
}

// If exactly one operand is an integer LITERAL and the operand widths differ,
// coerce the literal to the other operand's integer type. Go-faithful: only
// untyped literals adapt; two mismatched typed variables are left alone (that
// mismatch is a type error surfaced earlier, not silently coerced here).
static void coerce_int_literal_operand(CodeGenerator* codegen,
                                       ASTNode* left_ast, ValueInfo* left,
                                       ASTNode* right_ast, ValueInfo* right) {
    if (!left || !right || !left->goo_type || !right->goo_type) return;
    if (!type_is_integer(left->goo_type) || !type_is_integer(right->goo_type))
        return;
    LLVMTypeRef lt = LLVMTypeOf(left->llvm_value);
    LLVMTypeRef rt = LLVMTypeOf(right->llvm_value);
    if (LLVMGetTypeKind(lt) != LLVMIntegerTypeKind ||
        LLVMGetTypeKind(rt) != LLVMIntegerTypeKind) return;
    unsigned lw = LLVMGetIntTypeWidth(lt);
    unsigned rw = LLVMGetIntTypeWidth(rt);
    if (lw == rw) return;

    int left_lit  = left_ast  && left_ast->type  == AST_LITERAL;
    int right_lit = right_ast && right_ast->type == AST_LITERAL;
    if (left_lit == right_lit) return;  // neither, or both — leave alone

    if (left_lit) {
        left->llvm_value = int_widen_or_trunc(codegen, left->llvm_value, rt, lw, rw);
        left->goo_type   = right->goo_type;
    } else {
        right->llvm_value = int_widen_or_trunc(codegen, right->llvm_value, lt, rw, lw);
        right->goo_type   = left->goo_type;
    }
}
```

- [ ] **Step 5: Call the helper after both operands are loaded**

In `codegen_generate_binary_expr`, after the right-operand auto-load block (the `if (right_val->is_lvalue ...)` block ending ~line 514) and **before** "Get the result type from the type checker", insert:

```c
    // M3: reconcile an int literal operand's width to the other operand's
    // type so e.g. `int64_var == -1` compares i64-to-i64, not i64-to-i32.
    coerce_int_literal_operand(codegen, binary->left, left_val,
                               binary->right, right_val);
```

- [ ] **Step 6: Rebuild and run the probe — verify it PASSES**

Run:
```bash
make goo && make int64-probe
```
Expected: `int64-probe: PASS` with all three PASS lines matching.

- [ ] **Step 7: Run the full verify gate — no regressions**

Run:
```bash
make verify
```
Expected: every probe target prints PASS (the `verify: ALL GREEN GATES PASSED` banner). Ignore a failure isolated to `ccomp-build`.

- [ ] **Step 8: Adversarial review**

Dispatch the review-workflow (5 dimensions: finders → independent skeptic verifiers) over the diff. Pay special attention to: a real (non-literal) `i32` value accidentally widened; truncation of a large literal silently changing its value; float operands wrongly entering the integer path. Fix only verified findings.

- [ ] **Step 9: Commit and open the PR**

```bash
git checkout -b feat/m3-int64-coercion
git add src/codegen/expression_codegen.c examples/int64_probe.goo examples/int64_probe.expected.txt Makefile .github/workflows/tests.yml
git commit -m "feat(codegen): coerce int literals to the other operand's width

Integer literals were always emitted as i32, so any mix with an int64
value failed LLVM module verification (icmp/add i64-vs-i32). Coerce an
AST_LITERAL operand to the other operand's integer type, sign-aware
(SExt to widen, Trunc to narrow). Go-faithful: only untyped literals
adapt; two mismatched typed variables are left alone.

Closes the int64-vs-int-literal gap noted in examples/map_probe.goo.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
git push -u origin feat/m3-int64-coercion
gh pr create --fill
```
Then confirm CI green by reading `gh pr view --json statusCheckRollup` (not a piped exit).

---

## Phase 2 — comma-ok map reads `v, ok := m[k]`

Branch: `feat/m3-commaok-maps`. Depends on nothing from Phase 1; start from updated `main` after Phase 1 merges.

### Task 2: Presence-returning runtime getter

**Files:**
- Modify: `include/runtime.h` (declare after `goo_map_get_sv`, ~line 104)
- Modify: `src/runtime/runtime.c` (define after `goo_map_get_sv`, ~line 391)
- Test: `tests/runtime/test_map_get_ok.c` (new minimal C unit test; follow an existing `tests/` C test's structure)

**Interfaces:**
- Consumes: `GooMapSV`, `GooMapEntrySV{key, value, next}` (`include/runtime.h`, `src/runtime/runtime.c`).
- Produces: `void goo_map_get_sv_ok(GooMapSV* m, const char* k, int64_t* out, int* found);` — `*found=1` and `*out=value` if present, else `*found=0` and `*out=0`. Consumed by Task 3.

- [ ] **Step 1: Write the failing C unit test**

Create `tests/runtime/test_map_get_ok.c`:

```c
#include "runtime.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    GooMapSV* m = goo_map_new_sv();
    goo_map_set_sv(m, "a", 42);

    int64_t v = 999; int found = 7;
    goo_map_get_sv_ok(m, "a", &v, &found);
    assert(found == 1 && v == 42);

    v = 999; found = 7;
    goo_map_get_sv_ok(m, "missing", &v, &found);
    assert(found == 0 && v == 0);

    printf("test_map_get_ok: PASS\n");
    return 0;
}
```

- [ ] **Step 2: Compile the test — verify it FAILS**

Run:
```bash
make lib/libgoo_runtime.a
cc -Iinclude tests/runtime/test_map_get_ok.c lib/libgoo_runtime.a -o build/test_map_get_ok
```
Expected: link error — `undefined reference to 'goo_map_get_sv_ok'`.

- [ ] **Step 3: Declare and define the getter**

In `include/runtime.h`, after the `goo_map_get_sv` declaration (~line 104):

```c
// Presence-returning read: *found=1 and *out=value if k is present, else
// *found=0 and *out=0. Backs comma-ok map reads (v, ok := m[k]).
void goo_map_get_sv_ok(GooMapSV* m, const char* k, int64_t* out, int* found);
```

In `src/runtime/runtime.c`, after `goo_map_get_sv` (~line 391):

```c
void goo_map_get_sv_ok(GooMapSV* m, const char* k, int64_t* out, int* found) {
    *out = 0;
    *found = 0;
    if (!m || !k) return;
    GooMapEntrySV* e = (GooMapEntrySV*)m->head;
    while (e) {
        if (strcmp(e->key, k) == 0) {
            *out = e->value;
            *found = 1;
            return;
        }
        e = e->next;
    }
}
```

- [ ] **Step 4: Rebuild and run the test — verify it PASSES**

Run (header changed → clean rebuild of the lib):
```bash
make clean && make lib/libgoo_runtime.a
cc -Iinclude tests/runtime/test_map_get_ok.c lib/libgoo_runtime.a -o build/test_map_get_ok && ./build/test_map_get_ok
```
Expected: `test_map_get_ok: PASS`.

- [ ] **Step 5: Commit**

```bash
git checkout -b feat/m3-commaok-maps
git add include/runtime.h src/runtime/runtime.c tests/runtime/test_map_get_ok.c
git commit -m "feat(runtime): add goo_map_get_sv_ok presence-returning map getter

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

### Task 3: Typecheck + codegen for comma-ok reads

**Files:**
- Create: `examples/commaok_probe.goo`, `examples/commaok_probe.expected.txt`
- Modify: `src/types/type_checker.c` (multi-LHS var-decl path, ~line 436)
- Modify: `src/codegen/function_codegen.c` (multi-LHS var-decl codegen, ~line 255)
- Modify: `Makefile`, `.github/workflows/tests.yml:54`

**Interfaces:**
- Consumes: `goo_map_get_sv_ok` (Task 2); `VarDeclNode{names, name_count, values, is_short_decl}` (`include/ast.h:258`); `IndexExprNode{expr, index}` (`ast.h:444`); the existing multi-LHS `per_name_types` binding (`type_checker.c:436-449`) and `LLVMBuildExtractValue` destructure (`function_codegen.c:255-284`); `codegen_map_slot_to_value` (`composite_codegen.c`); `type_bool()` (`src/types/types.c:42`); `TYPE_STRUCT` construction (follow `struct_type` usage in `src/types/types.c`).
- Produces: comma-ok read semantics; nothing consumed by Phase 3.

- [ ] **Step 1: Write the failing probe**

Create `examples/commaok_probe.goo`:

```goo
// commaok_probe: `v, ok := m[k]` distinguishes a present key from an absent
// one, so the zero value and a missing key are no longer ambiguous.
package main

import "fmt"

func main() {
    ages := map[string]int{"alice": 30}

    v, ok := ages["alice"]
    if ok {
        fmt.Println("PASS: present key ok=true")
    }
    fmt.Println(v)

    z, ok2 := ages["missing"]
    if ok2 {
        fmt.Println("FAIL: absent key reported present")
    }
    fmt.Println(z)
}
```

Create `examples/commaok_probe.expected.txt`:

```
PASS: present key ok=true
30
0
```

(The absent line prints no `FAIL` marker and `z` is `0`, proving `ok2` is false and the value is the zero value. Uses only `if cond {` + `fmt.Println`, no boolean operators, to stay within confirmed surface.)

- [ ] **Step 2: Wire the probe into the build**

Add a `commaok-probe` target to `Makefile` (copy the `int64-probe` recipe from Phase 1, substituting `commaok`); append ` commaok-probe` to `verify:` and to `.github/workflows/tests.yml:54`.

- [ ] **Step 3: Build and run — verify it FAILS**

Run:
```bash
make goo && make commaok-probe
```
Expected: FAIL at typecheck/codegen — the 2-LHS decl expects a 2-field struct RHS, but a map index currently yields a bare value, so name binding or destructure fails.

- [ ] **Step 4: Typecheck — synthesize `{V, bool}` for a map-index 2-LHS RHS**

Read `src/types/type_checker.c:420-460` (the multi-LHS var-decl block) first. In that block, before the existing `per_name_types` computation, add a special case: when `var_decl->name_count == 2`, `var_decl->is_short_decl`, and `var_decl->values->type == AST_INDEX_EXPR` whose base type-checks to `TYPE_MAP`, set `final_type` to a freshly built `TYPE_STRUCT` with two fields — field 0 = the map's `value_type`, field 1 = `type_bool()`. Build the struct type following the `struct_type` construction already used in `src/types/types.c` (a 2-element `fields` array; names `"v"`/`"ok"` are fine). The existing `per_name_types` loop (`type_checker.c:436-449`) then binds name 0 → V and name 1 → bool unchanged.

Concretely, the guard condition to add:

```c
// comma-ok map read: `v, ok := m[k]` — the RHS map index yields {V, bool}.
if (var_decl->name_count == 2 && var_decl->is_short_decl &&
    var_decl->values && var_decl->values->type == AST_INDEX_EXPR) {
    ASTNode* base = ((IndexExprNode*)var_decl->values)->expr;
    Type* bt = type_check_expression(checker, base);
    if (bt && bt->kind == TYPE_MAP) {
        Type* fields[2] = { bt->data.map.value_type, type_bool() };
        final_type = type_struct_anon(fields, 2);  // build per struct_type pattern
    }
}
```
If no `type_struct_anon`-style helper exists, construct the `TYPE_STRUCT` inline exactly as `struct_type` literals are built elsewhere in `types.c`. Do not alter the single-LHS map-index path (`v := m[k]` must still yield bare V).

- [ ] **Step 5: Codegen — lower the 2-LHS map read via `goo_map_get_sv_ok`**

Read `src/codegen/function_codegen.c:255-284` (multi-LHS destructure) and `src/codegen/composite_codegen.c:38-60` (map index lowering) first. In the multi-LHS var-decl codegen, before the generic `codegen_generate_expression(...)` RHS evaluation, add a branch: when `name_count == 2` and `values` is an `AST_INDEX_EXPR` over a map, lower the read explicitly instead:

```c
// comma-ok map read: build a {value, ok} aggregate from goo_map_get_sv_ok,
// then fall into the existing ExtractValue destructure below.
//  1. Evaluate the map pointer (index_expr->expr) and the key (index_expr->index).
//  2. alloca i64 out_slot, i32 found_slot.
//  3. LLVMBuildCall2 goo_map_get_sv_ok(map, key, out_slot, found_slot)
//     (declare the fn via LLVMGetNamedFunction / add to module like
//      goo_map_get_sv is referenced at composite_codegen.c:42).
//  4. load out_slot -> i64; convert to V with codegen_map_slot_to_value.
//  5. load found_slot -> i32; truncate to i1 (LLVMBuildTrunc) for bool.
//  6. LLVMBuildInsertValue into an undef {V, i1} struct to form `rhs`.
```

Bind the resulting `{V, bool}` aggregate through the **existing** `LLVMBuildExtractValue` loop (`function_codegen.c:255-284`) — name 0 → `v`, name 1 → `ok`. Implement the six sub-steps with the real LLVM C API calls listed; reuse the `goo_map_get_sv` reference pattern at `composite_codegen.c:42` for obtaining/declaring the function. Leave the single-value map-index path untouched.

- [ ] **Step 6: Rebuild and run — verify it PASSES**

Run (headers in Task 2 already landed; if any header touched here, `make clean` first):
```bash
make goo && make commaok-probe
```
Expected: `commaok-probe: PASS` with output exactly `PASS: present key ok=true` / `30` / `0`.

- [ ] **Step 7: Regression gate**

Run:
```bash
make verify
```
Expected: all probes PASS, including the unchanged `map-probe` (single-value reads must still work). Ignore `ccomp-build`.

- [ ] **Step 8: Adversarial review**

Run the review-workflow over the diff. Focus: single-LHS `v := m[k]` regressions; the `found` i32→i1 truncation; map pointer/key evaluation order; non-map 2-LHS decls (e.g. `a, b := f()`) still routing to the struct path, not the map path.

- [ ] **Step 9: Commit and open the PR**

```bash
git add examples/commaok_probe.goo examples/commaok_probe.expected.txt src/types/type_checker.c src/codegen/function_codegen.c Makefile .github/workflows/tests.yml
git commit -m "feat(maps): comma-ok reads v, ok := m[k]

A map index in a 2-LHS short-decl now yields {V, bool} via the new
goo_map_get_sv_ok getter, reusing the existing multi-LHS struct
destructure. Single-value reads (v := m[k]) are unchanged. := only;
plain multi-assign (v, ok = m[k]) is deferred.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
git push -u origin feat/m3-commaok-maps
gh pr create --fill
```
Confirm CI green via `gh pr view --json statusCheckRollup`.

---

## Phase 3 — match guard lowering `case X{..} if cond:`

Branch: `feat/m3-match-guards`. Start from updated `main` after Phase 2 merges.

### Task 4: Evaluate the guard and branch to the fallback arm

**Files:**
- Create: `examples/guard_probe.goo`, `examples/guard_probe.expected.txt`
- Modify: `src/codegen/composite_codegen.c` (`codegen_generate_match`, 448-602)
- Modify: `Makefile`, `.github/workflows/tests.yml:54`

**Interfaces:**
- Consumes: `MatchCaseNode{pattern, guard, body}` (`include/ast.h:726`); `GuardConditionNode{condition}` (`ast.h:755`); `codegen_generate_expression`; the per-arm basic-block + switch structure (`composite_codegen.c:482-597`); bound payload field locals (registered at lines 541-580); LLVM C API (`LLVMAppendBasicBlock`, `LLVMBuildCondBr`, `LLVMPositionBuilderAtEnd`).
- Produces: guard-gated `match` arms. Self-contained.

- [ ] **Step 1: Write the failing probe**

Create `examples/guard_probe.goo` (single-variant enum so no same-tag arms; guard-false falls to `default`):

```goo
// guard_probe: a match-arm guard (`case V{x} if cond:`) gates the arm. When the
// guard is false, control falls through to the default arm.
package main

import "fmt"

type Box enum {
    Val{n: int}
}

func classify(b *Box) {
    match *b {
    case Val{n} if n > 0:
        fmt.Println("positive")
    default:
        fmt.Println("not-positive")
    }
}

func main() {
    p := new(Box)
    *p = Val{n: 5}
    classify(p)

    q := new(Box)
    *q = Val{n: -3}
    classify(q)
}
```

Create `examples/guard_probe.expected.txt`:

```
positive
not-positive
```

(`Val{5}` → guard true → `positive`; `Val{-3}` → guard false → falls to `default` → `not-positive`.)

- [ ] **Step 2: Wire the probe into the build**

Add a `guard-probe` target to `Makefile` (copy the `int64-probe` recipe, substituting `guard`); append ` guard-probe` to `verify:` and `.github/workflows/tests.yml:54`.

- [ ] **Step 3: Build and run — verify it FAILS**

Run:
```bash
make goo && make guard-probe
```
Expected: FAIL — guard is ignored, so `Val{-3}` prints `positive` instead of `not-positive` (output is `positive` / `positive`).

- [ ] **Step 4: Resolve the fallback block before per-arm lowering**

Read `codegen_generate_match` (`composite_codegen.c:448-602`) first. Before the per-arm loop, determine the **fallback block** a failed guard jumps to: the basic block of the `default`/wildcard arm if the match has one, otherwise the match's merge/continuation block (the block control reaches after the `switch`). Capture it in a local `LLVMBasicBlockRef guard_fallback_bb`. (The wildcard arm and merge block are already created in this function for the switch's default destination — reuse that block; do not create a second one.)

- [ ] **Step 5: Evaluate the guard after payload binding, before the body**

In the per-arm lowering, at the injection point after payload binding (line 580) and before body emission (line 586), insert:

```c
    // M3: a guarded arm runs its body only if the guard holds; otherwise
    // control falls through to the default arm (or the merge block).
    if (mc->guard) {
        ValueInfo* g = codegen_generate_expression(
            codegen, checker, ((GuardConditionNode*)mc->guard)->condition);
        if (g && g->llvm_value) {
            LLVMBasicBlockRef body_bb = LLVMAppendBasicBlock(
                codegen->current_function, "guard_body");
            LLVMBuildCondBr(codegen->builder, g->llvm_value,
                            body_bb, guard_fallback_bb);
            LLVMPositionBuilderAtEnd(codegen->builder, body_bb);
        }
    }
```

The guard expression sees the payload field locals bound just above (e.g. `n`), matching the typechecker's arm-scope evaluation. Body emission (lines 586-588) then runs in `guard_body`. Ensure the bound `i1` from the comparison is used directly as the `CondBr` condition.

- [ ] **Step 6: Rebuild and run — verify it PASSES**

Run:
```bash
make goo && make guard-probe
```
Expected: `guard-probe: PASS`, output exactly `positive` / `not-positive`.

- [ ] **Step 7: Regression gate**

Run:
```bash
make verify
```
Expected: all probes PASS, including the unchanged `match-probe` (guardless arms must be unaffected). Ignore `ccomp-build`.

- [ ] **Step 8: Adversarial review**

Run the review-workflow over the diff. Focus: a guardless arm accidentally gaining a spurious branch; the fallback block being the wrong target when no `default` exists (must be the merge block, a no-op); builder left positioned in the wrong block after the arm; guard referencing an unbound name.

- [ ] **Step 9: Commit and open the PR**

```bash
git add examples/guard_probe.goo examples/guard_probe.expected.txt src/codegen/composite_codegen.c Makefile .github/workflows/tests.yml
git commit -m "feat(match): lower match-arm guards (case X{..} if cond:)

The guard parsed and typechecked but was never lowered. Evaluate it
after payload binding; on false, branch to the default arm (or the
merge block when there is none). Same-tag guarded arms remain
unsupported (tag-switch dispatch) — documented as a known limitation.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
git push -u origin feat/m3-match-guards
gh pr create --fill
```
Confirm CI green via `gh pr view --json statusCheckRollup`.

---

## Post-milestone

After all three PRs merge, record the known limitations (from the spec's "Known limitations" section) in the spec doc's status and update the handoff/memory. The deferred follow-ups are: `v, ok = m[k]` plain multi-assign; same-tag guarded `match` arms (needs tag-switch → per-tag if-else dispatch restructure).

## Self-Review

**Spec coverage:** F1 (literal coercion, sign-aware, literals-only) → Task 1. F2 (runtime getter → Task 2; typecheck `{V,bool}` + codegen aggregate, `:=`-only → Task 3). F3 (guard eval after payload binding, branch to default/merge; same-tag limitation documented) → Task 4. Test gates (probe + Makefile + verify + CI per feature) → present in every task. Sequencing (3 PRs, int64 first) → phase order. All spec sections map to a task.

**Placeholder scan:** No "TBD"/"implement later". Phase 1 and the runtime getter are verbatim. Phase 2 Task 3 Steps 4-5 and Phase 3 Step 5 give concrete code plus a "read the named function first" instruction with exact insertion points and real API calls — concrete algorithms, not placeholders, reflecting that the exact surrounding lines are verified at execution time (suits subagent-driven execution with fresh recon).

**Type consistency:** `goo_map_get_sv_ok(GooMapSV*, const char*, int64_t*, int*)` is declared (Task 2) and consumed identically (Task 3). `coerce_int_literal_operand`/`int_widen_or_trunc` names are consistent across definition and call (Task 1). `ValueInfo{llvm_value, goo_type}`, `binary->left/right`, `mc->guard`, `GuardConditionNode.condition` match the recon-verified field names.
