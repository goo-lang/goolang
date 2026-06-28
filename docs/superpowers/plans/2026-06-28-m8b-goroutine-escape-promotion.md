# M8b — Goroutine-Escaping Local Heap-Promotion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Heap-promote stack locals whose address escapes into a `go` statement, so a goroutine spawned from a non-`main` frame can safely read them after that frame returns.

**Architecture:** Codegen-only. A per-function AST pre-pass computes which named locals to promote (address-taken **and** the function contains a `go`). A `codegen_alloc_local` helper allocates promoted locals with `goo_alloc` (leak-consistent) instead of a stack `alloca`; the two user-local creation sites (params, var/short decls) route through it. Everything downstream is unchanged because locals are already pointers-to-storage. No runtime or grammar change.

**Tech Stack:** C23, LLVM 22 C API, pthread runtime, GNU Make, bash diff-based probe harness.

## Global Constraints

- **Promotion rule (sound trigger):** promote local `L` iff (a) `L`'s address is taken anywhere in the function (`&L`, `&L.field`, `&L[i]` — root local `L`) AND (b) the function contains ≥1 `go` statement (`AST_GO_STMT`). Detecting the `&` operation itself covers aliasing (`p := &x; go f(p)`) — verbatim from spec §4.
- **Leak-consistent:** promoted locals use `goo_alloc` and are never freed (matches the existing allocate-and-leak model, `composite_codegen.c:702-708`). No runtime change.
- **Scope to `go`-containing functions only.** A `&local`-without-`go` function is untouched.
- **Over-promotion accepted:** an address-taken local in a `go`-containing function is promoted even if it does not actually reach the `go`. Safe; extra leak only.
- **No header change.** Pre-pass state is file-static in `src/codegen/function_codegen.c` (function codegen is non-reentrant; Goo has no nested function decls). Both promotion sites already live in this file.
- **Build facts:** compiler `bin/goo`, runtime `lib/libgoo_runtime.a`. Build: `make goo lib/libgoo_runtime.a`. No `include/*.h` edits in this plan, so `make clean` is not required. `make verify` halts at `ccomp-build` (CompCert gap) — ignore; the real gate is the CI probe list.
- **Run concurrency probes under a timeout** (`timeout 10 ./build/<probe>`) so a hang fails loudly.
- **CI wiring:** the new probe goes in BOTH `verify:` (Makefile) AND `.github/workflows/tests.yml:54`.
- **CI billing caveat:** Actions on `dd0wney/goolang` may be billing-blocked (jobs show red, never start). Authoritative verification is LOCAL: the probe list + `opt --passes=verify` + `make test`.
- **clang/LSP false positives:** "header not found" diagnostics are not real (build uses `-Iinclude`). Trust `make`.

## Reference map

| Concern | Location | Notes |
|---|---|---|
| Param local creation | `src/codegen/function_codegen.c:194` | `codegen_create_entry_alloca(codegen, param_types[i], param_name)` then store + `codegen_add_value` |
| var/short-decl local creation | `src/codegen/function_codegen.c:362-396` | single-LHS path; `:=` is a `VarDeclNode` with `is_short_decl=1` routed here; alloca at `:379`, zero-init at `:380-395` |
| Function decl entry point (add pre-pass here) | `src/codegen/function_codegen.c` `codegen_generate_function_decl` | the function that emits a function body; body AST is the `FuncDeclNode.body` |
| entry-alloca helper (the stack path) | `src/codegen/codegen.c:446` `codegen_create_entry_alloca` | save/restore builder position; allocas go in entry block |
| `goo_alloc` on-demand declare pattern | `src/codegen/composite_codegen.c:710-715` | `LLVMGetNamedFunction` or `LLVMAddFunction`; `LLVMSizeOf(type)` → i64; `LLVMBuildCall2` |
| AST node fields | `include/ast.h` | `GoStmtNode.call`; `UnaryExprNode.{operator,operand}`; `IdentifierNode.name`; `SelectorExprNode.expr`; `IndexExprNode.{expr,index}`; `CallExprNode.{function,args}`; `BlockStmtNode.statements`; `ExprStmtNode.expr`; `IfStmtNode.{condition,then_stmt,else_stmt}`; `ForStmtNode.{init,condition,post,body,range_expr}`; `ReturnStmtNode.values`; `VarDeclNode.values`; `BinaryExprNode.{left,right}`; `PostfixExprNode.operand`; `DeferStmtNode.call`; `SwitchStmtNode.{tag,cases}`; `CaseClauseNode.{exprs,body}`; `SelectStmtNode.cases`; `SelectCaseNode.{comm,body}`; `IfLetStmtNode.{nullable_expr,then_stmt,else_stmt}` |
| `&` operator token | `include/ast.h` / lexer | `TOKEN_BIT_AND` (used at `expression_codegen.c:932`) |
| probe + Makefile pattern to copy | `examples/go_probe.goo`, Makefile `go-probe:` (~`:652`), `verify:` (~`:760`) | M8 goroutine probe |
| tests.yml probe list | `.github/workflows/tests.yml:54` | space-separated make targets |

---

## Task 1: Failing `escape-probe` (RED)

**Files:**
- Create: `examples/escape_probe.goo`, `examples/escape_probe.expected.txt`

**Interfaces:**
- Consumes: M8 goroutine spawning (`go f(...)` multi-arg thunking), channels (`make_chan`, `<-`), pointer deref-load (`*p`), address-of (`&x`).
- Produces: the probe the rest of the plan turns green.

- [ ] **Step 1: Write the probe program**

Create `examples/escape_probe.goo`:

```go
// escape-probe: a local whose address escapes into a goroutine spawned from a
// NON-main frame must survive after that frame returns. main-spawned locals are
// already safe via the #29 run-to-completion barrier, so the spawn MUST be in a
// helper. Two cases: direct `&x` and aliased `p := &x`. The channel receive is
// the deterministic join.
package main

import "fmt"

func reader(p *int, done chan int) {
    done <- *p
}

func spawnDirect(done chan int) {
    x := 7
    go reader(&x, done)
}

func spawnAliased(done chan int) {
    y := 9
    p := &y
    go reader(p, done)
}

func main() {
    d1 := make_chan(int, 1)
    spawnDirect(d1)
    fmt.Println(<-d1)

    d2 := make_chan(int, 1)
    spawnAliased(d2)
    fmt.Println(<-d2)
}
```

Create `examples/escape_probe.expected.txt`:

```
7
9
```

- [ ] **Step 2: Build the toolchain and run the probe to confirm it fails**

Run:
```bash
make goo lib/libgoo_runtime.a
mkdir -p build
bin/goo -o build/escape_probe examples/escape_probe.goo
timeout 10 ./build/escape_probe | diff -u examples/escape_probe.expected.txt -
echo "exit: ${PIPESTATUS[0]} / diff: $?"
```

Expected: **FAIL** — the program compiles (M8 already supports `go reader(&x, done)`), but `&x`/`&y` point to `spawnDirect`/`spawnAliased` stack slots that are gone by the time `reader` runs, so the printed values are garbage (not `7`/`9`), or it crashes. Capture this as RED. (If by chance the stale stack happens to still read `7`/`9`, treat as inconclusive and proceed — Task 3's IR assertion is the real proof; note it in the commit.)

- [ ] **Step 3: Commit the failing probe**

```bash
git add examples/escape_probe.goo examples/escape_probe.expected.txt
git commit --no-gpg-sign -m "test(concurrency): add escape-probe (RED) — &local escaping into go from a non-main frame"
```

---

## Task 2: Escape pre-pass + heap-promoting allocation (probe GREEN)

**Files:**
- Modify: `src/codegen/function_codegen.c` (add file-static pre-pass + `codegen_alloc_local`; route the two local-creation sites through it; call the pre-pass at function entry)

**Interfaces:**
- Consumes: AST node fields (Reference map); `codegen_create_entry_alloca` (`codegen.c:446`); `goo_alloc` declare pattern (`composite_codegen.c:710`).
- Produces (file-static, used only within `function_codegen.c`):
  - `static void escape_prepass_compute(ASTNode* body);` — fills the promotion set for the current function.
  - `static int escape_is_promoted(const char* name);` — 1 if `name` is promoted.
  - `static LLVMValueRef codegen_alloc_local(CodeGenerator* codegen, LLVMTypeRef type, const char* name);` — heap or stack storage pointer.

- [ ] **Step 1: Add the file-static pre-pass state and AST walkers**

At the top of `src/codegen/function_codegen.c` (after the existing `#include`s), add:

```c
// ---- M8b: goroutine-escape pre-pass (file-static; function codegen is
// non-reentrant and Goo has no nested function decls) --------------------------

#define ESCAPE_MAX_NAMES 256
static const char* g_escape_names[ESCAPE_MAX_NAMES];
static size_t g_escape_count;
static int g_escape_has_go;

// Root local name of an lvalue whose address is taken: descend through selector
// (`x.f`) and index (`x[i]`) to the underlying identifier. (Parens are folded
// away by the parser — there is no paren-expr node — and AST_PAREN_EXPR is a
// repurposed slot for map literals, which are not addressable lvalues.)
static const char* escape_root_local(ASTNode* lv) {
    while (lv) {
        switch (lv->type) {
            case AST_IDENTIFIER: return ((IdentifierNode*)lv)->name;
            case AST_SELECTOR_EXPR: lv = ((SelectorExprNode*)lv)->expr; break;
            case AST_INDEX_EXPR:    lv = ((IndexExprNode*)lv)->expr; break;
            default: return NULL;
        }
    }
    return NULL;
}

static void escape_add(const char* name) {
    if (!name) return;
    for (size_t i = 0; i < g_escape_count; i++)
        if (strcmp(g_escape_names[i], name) == 0) return;  // dedup
    if (g_escape_count < ESCAPE_MAX_NAMES)
        g_escape_names[g_escape_count++] = name;
}

// Recursively visit a node and its `next`-chained siblings, recording any `&`
// address-of root local and noting whether a `go` statement appears.
static void escape_walk(ASTNode* n) {
    for (; n; n = n->next) {
        switch (n->type) {
            case AST_GO_STMT: {
                g_escape_has_go = 1;
                escape_walk(((GoStmtNode*)n)->call);
                break;
            }
            case AST_UNARY_EXPR: {
                UnaryExprNode* u = (UnaryExprNode*)n;
                if (u->operator == TOKEN_BIT_AND)
                    escape_add(escape_root_local(u->operand));
                escape_walk(u->operand);
                break;
            }
            case AST_BLOCK_STMT: escape_walk(((BlockStmtNode*)n)->statements); break;
            case AST_EXPR_STMT:  escape_walk(((ExprStmtNode*)n)->expr); break;
            case AST_IF_STMT: {
                IfStmtNode* s = (IfStmtNode*)n;
                escape_walk(s->condition); escape_walk(s->then_stmt); escape_walk(s->else_stmt);
                break;
            }
            case AST_IF_LET_STMT: {
                IfLetStmtNode* s = (IfLetStmtNode*)n;
                escape_walk(s->nullable_expr); escape_walk(s->then_stmt); escape_walk(s->else_stmt);
                break;
            }
            case AST_FOR_STMT: {
                ForStmtNode* s = (ForStmtNode*)n;
                escape_walk(s->init); escape_walk(s->condition); escape_walk(s->post);
                escape_walk(s->range_expr); escape_walk(s->body);
                break;
            }
            case AST_RETURN_STMT: escape_walk(((ReturnStmtNode*)n)->values); break;
            case AST_VAR_DECL:    escape_walk(((VarDeclNode*)n)->values); break;
            case AST_DEFER_STMT:  escape_walk(((DeferStmtNode*)n)->call); break;
            case AST_BINARY_EXPR: {
                BinaryExprNode* b = (BinaryExprNode*)n;
                escape_walk(b->left); escape_walk(b->right);
                break;
            }
            case AST_POSTFIX_EXPR: escape_walk(((PostfixExprNode*)n)->operand); break;
            // (AST_PAREN_EXPR is a repurposed map-literal slot, not a wrapper —
            // a map-literal value position holding `&x` is rare; left to the
            // default no-op, which only ever under-promotes, never wrongly.)
            case AST_CALL_EXPR: {
                CallExprNode* c = (CallExprNode*)n;
                escape_walk(c->function); escape_walk(c->args);
                break;
            }
            case AST_INDEX_EXPR: {
                IndexExprNode* ix = (IndexExprNode*)n;
                escape_walk(ix->expr); escape_walk(ix->index);
                break;
            }
            case AST_SELECTOR_EXPR: escape_walk(((SelectorExprNode*)n)->expr); break;
            case AST_SWITCH_STMT: {
                SwitchStmtNode* s = (SwitchStmtNode*)n;
                escape_walk(s->tag); escape_walk(s->cases);
                break;
            }
            case AST_CASE_CLAUSE: {
                CaseClauseNode* c = (CaseClauseNode*)n;
                escape_walk(c->exprs); escape_walk(c->body);
                break;
            }
            case AST_SELECT_STMT: escape_walk(((SelectStmtNode*)n)->cases); break;
            case AST_SELECT_CASE: {
                SelectCaseNode* c = (SelectCaseNode*)n;
                escape_walk(c->comm); escape_walk(c->body);
                break;
            }
            default: break;  // leaves (identifier, literal, types): nothing to recurse
        }
    }
}

static void escape_prepass_compute(ASTNode* body) {
    g_escape_count = 0;
    g_escape_has_go = 0;
    escape_walk(body);
    if (!g_escape_has_go) g_escape_count = 0;  // promote only in go-containing functions
}

static int escape_is_promoted(const char* name) {
    if (!name) return 0;
    for (size_t i = 0; i < g_escape_count; i++)
        if (strcmp(g_escape_names[i], name) == 0) return 1;
    return 0;
}
```

NOTE: all enum names above are verified present in `include/ast.h` for this tree. If a future build reports an unknown node-type or field, grep `include/ast.h` for the actual name and adjust that one case. The walker's correctness depends only on covering `AST_GO_STMT` and `AST_UNARY_EXPR(&)`; the other cases exist solely to recurse into children, so a missing recurse-only case degrades gracefully to less promotion (caught by Step 5's IR check), never to a wrong promotion.

- [ ] **Step 2: Add the heap-promoting allocation helper**

Immediately below the pre-pass code in `src/codegen/function_codegen.c`, add:

```c
// Allocate storage for a named local: heap (goo_alloc, leaked) if the local is
// goroutine-escape-promoted, else a stack entry alloca. Under opaque pointers
// both return `ptr`, so all downstream loads/stores (which carry explicit types)
// are unchanged.
static LLVMValueRef codegen_alloc_local(CodeGenerator* codegen, LLVMTypeRef type, const char* name) {
    if (!escape_is_promoted(name))
        return codegen_create_entry_alloca(codegen, type, name);

    LLVMContextRef ctx = codegen->context;
    LLVMTypeRef i64t = LLVMInt64TypeInContext(ctx);
    LLVMTypeRef vp = LLVMPointerType(LLVMInt8TypeInContext(ctx), 0);
    LLVMTypeRef alloc_ty = LLVMFunctionType(vp, &i64t, 1, 0);
    LLVMValueRef alloc_fn = LLVMGetNamedFunction(codegen->module, "goo_alloc");
    if (!alloc_fn) alloc_fn = LLVMAddFunction(codegen->module, "goo_alloc", alloc_ty);

    // Emit the goo_alloc in the entry block (like an alloca) so it dominates all
    // uses and runs once per call. Save/restore the builder position.
    LLVMBasicBlockRef cur = LLVMGetInsertBlock(codegen->builder);
    LLVMBasicBlockRef entry = codegen->current_function_info
        ? codegen->current_function_info->entry_block
        : LLVMGetEntryBasicBlock(codegen->current_function);
    LLVMValueRef first = entry ? LLVMGetFirstInstruction(entry) : NULL;
    if (first) LLVMPositionBuilderBefore(codegen->builder, first);
    else       LLVMPositionBuilderAtEnd(codegen->builder, entry);

    LLVMValueRef size = LLVMSizeOf(type);
    LLVMValueRef p = LLVMBuildCall2(codegen->builder, alloc_ty, alloc_fn, &size, 1,
                                    name ? name : "go_escape_local");
    if (cur) LLVMPositionBuilderAtEnd(codegen->builder, cur);
    return p;
}
```

- [ ] **Step 3: Run the pre-pass at function entry**

In `codegen_generate_function_decl` (`src/codegen/function_codegen.c`), find where the function body begins to be generated (after `current_function`/`entry_block` are set up, before params and body statements are emitted). Add a call to compute the promotion set from the function body. The body node is the `FuncDeclNode.body` field. Insert:

```c
    // M8b: compute which locals escape into a goroutine and must be heap-promoted.
    escape_prepass_compute(func_decl->body);
```

(Use the actual local variable name for the `FuncDeclNode*` in scope — grep the function header for the cast, commonly `func_decl` or `func`. It must run before the param allocas at line ~194 and the body var-decls.)

- [ ] **Step 4: Route the two local-creation sites through the helper**

Site A — params (`src/codegen/function_codegen.c:194`). Replace:

```c
                LLVMValueRef param_alloca = codegen_create_entry_alloca(codegen, param_types[param_index], param_name);
```

with:

```c
                LLVMValueRef param_alloca = codegen_alloc_local(codegen, param_types[param_index], param_name);
```

Site B — var/short decl single-LHS (`src/codegen/function_codegen.c:379`). Replace:

```c
            alloca_inst = codegen_create_entry_alloca(codegen, llvm_type, var_name);
```

with:

```c
            alloca_inst = codegen_alloc_local(codegen, llvm_type, var_name);
```

(Leave the zero-init store block at `:380-395` unchanged — storing into a heap `ptr` works identically.)

- [ ] **Step 5: Build, run the probe, and verify it passes + IR shows goo_alloc**

Run:
```bash
make goo lib/libgoo_runtime.a
bin/goo -o build/escape_probe examples/escape_probe.goo
timeout 10 ./build/escape_probe | diff -u examples/escape_probe.expected.txt -
echo "exit: ${PIPESTATUS[0]} / diff: $?"
bin/goo --emit-llvm examples/escape_probe.goo >/dev/null 2>&1
grep -c "call ptr @goo_alloc" examples/escape_probe.out.ll
OPT=$(command -v opt || command -v opt-22); "$OPT" --passes=verify -disable-output examples/escape_probe.out.ll && echo "IR verify CLEAN"
```

Expected: no diff, exit 0 (not 124/timeout); `grep -c` ≥ 2 (the promoted `x` and `y`); IR verify clean. If the count is 0, the pre-pass isn't firing — re-check Step 3's body field name and that `TOKEN_BIT_AND` is the `&` token in this tree.

- [ ] **Step 6: Commit**

```bash
git add src/codegen/function_codegen.c
git commit --no-gpg-sign -m "feat(concurrency): heap-promote goroutine-escaping locals (escape-probe GREEN)"
```

---

## Task 3: Scope guard — a `&`-taken local in a `go`-free function stays on the stack

**Files:**
- Create: `examples/escape_nogo_probe.goo` (control; compile-and-IR only, no runtime assertion needed)

**Interfaces:**
- Consumes: Task 2's promotion (must NOT fire here).
- Produces: evidence for spec scope-decision #3 (promotion is scoped to `go`-containing functions).

- [ ] **Step 1: Write the control program**

Create `examples/escape_nogo_probe.goo`:

```go
// Control for M8b scope: `&local` in a function with NO `go` must NOT be
// promoted — the local stays a stack alloca. (Compile + IR check only.)
package main

import "fmt"

func usePtr(p *int) int {
    return *p
}

func main() {
    n := 5
    fmt.Println(usePtr(&n))   // &n taken, but main() has no `go` → no promotion
}
```

- [ ] **Step 2: Build, emit IR, and assert `n` is a stack alloca (not goo_alloc)**

Run:
```bash
bin/goo -o build/escape_nogo_probe examples/escape_nogo_probe.goo
timeout 10 ./build/escape_nogo_probe   # prints 5
bin/goo --emit-llvm examples/escape_nogo_probe.goo >/dev/null 2>&1
echo "goo_alloc count (want 0): $(grep -c 'call ptr @goo_alloc' examples/escape_nogo_probe.out.ll)"
echo "alloca for n (want >=1):  $(grep -c 'alloca' examples/escape_nogo_probe.out.ll)"
```

Expected: prints `5`; `goo_alloc` count `0` (no `go` in the file → nothing promoted); at least one `alloca` present. This proves the scope guard.

- [ ] **Step 3: Commit**

```bash
git add examples/escape_nogo_probe.goo
git commit --no-gpg-sign -m "test(concurrency): control probe — &local without go stays on the stack"
```

---

## Task 4: Wire probes into the gate + full regression

**Files:**
- Modify: `Makefile` (add `escape-probe:` target; append to `verify:`)
- Modify: `.github/workflows/tests.yml:54` (append `escape-probe`)

**Interfaces:**
- Consumes: `examples/escape_probe.*`.
- Produces: CI-gated `escape-probe`.

- [ ] **Step 1: Add the Makefile probe target**

In `Makefile`, after the existing `go-probe:` target, add (use TAB indentation, matching the file):

```make
# M8b escape-probe: a local whose address escapes into a goroutine spawned from
# a non-main frame survives after that frame returns (heap-promotion).
escape-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== escape-probe: &local escaping into go from a non-main frame is heap-promoted ==="
	$(COMPILER) -o build/escape_probe examples/escape_probe.goo
	@timeout 10 ./build/escape_probe > build/escape_probe.actual.txt; rc=$$?; \
	if [ $$rc -ne 0 ]; then echo "escape-probe: FAIL (exit $$rc — hang or crash)"; exit 1; fi
	@if diff -u examples/escape_probe.expected.txt build/escape_probe.actual.txt; then \
	  echo "escape-probe: PASS"; \
	else \
	  echo "escape-probe: FAIL (see diff above)"; \
	  exit 1; \
	fi
```

- [ ] **Step 2: Append `escape-probe` to the `verify:` target**

In `Makefile`, find the `verify:` line (~`:760`) and append ` escape-probe` to the end of its dependency list (after `go-probe`).

- [ ] **Step 3: Append `escape-probe` to the CI probe list**

In `.github/workflows/tests.yml:54`, append ` escape-probe` to the end of the space-separated make-target list (after `go-probe`).

- [ ] **Step 4: Run the new target and the full regression gate**

Run:
```bash
make escape-probe
make go-probe chan-probe chan-elem-probe chan-padded-probe chan-uint-probe block-scope-probe 2>&1 | grep -E "PASS|FAIL"
make test 2>&1 | grep -E "Tests run|Passed|Failed|All tests"
```

Expected: `escape-probe: PASS`; every regression probe `PASS`; `make test` shows no new failures (76 passed / 1 skipped baseline). The `go-probe` and channel probes are the key guard — M8b sits on the goroutine path.

- [ ] **Step 5: Commit**

```bash
git add Makefile .github/workflows/tests.yml
git commit --no-gpg-sign -m "ci(concurrency): gate escape-probe in verify: and tests.yml"
```

---

## Final verification

- [ ] `escape-probe` green under timeout (exit 0, output `7`/`9`); `opt --passes=verify` clean.
- [ ] IR shows ≥2 `goo_alloc` for the promoted locals in `escape_probe`; `escape_nogo_probe` shows `0` `goo_alloc` and a stack `alloca` (scope guard).
- [ ] Aliased `p := &y` case passes (proves the `&`-detection-covers-aliasing claim).
- [ ] Full CI probe gate + `make test` green — no regressions (esp. `go-probe` + channel probes).
- [ ] Spec §10 success criteria met.

## Spec coverage self-check

| Spec §10 / §4 element | Task |
|---|---|
| promote address-taken ∧ go-containing local | 2 |
| `&`-detection covers aliasing (`p := &x`) | 1 (aliased case), 2 |
| heap-promote via goo_alloc (leak-consistent) | 2 |
| allocation-site-only mechanism | 2 (helper + 2 sites) |
| non-main-frame probe rationale | 1 |
| scope guard: no-go function unchanged | 3 |
| IR assertions (goo_alloc / alloca) | 2, 3 |
| under timeout, wired into verify: + tests.yml | 4 |
| no regressions / local verification | 4 |
