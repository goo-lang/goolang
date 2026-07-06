# Function Generics Tier A Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make ordinary generic functions (`func Map[T, U any](s []T, f func(T) U) []U`) parse, type-check with inferred instantiation, and run via monomorphized codegen.

**Architecture:** Four layers. (1) Grammar+AST gain an optional `[type-params]` clause on `func` decls. (2) The type checker scopes each type param as a `TYPE_PARAM` `Type*`, checks the body once abstractly, and at each call infers concrete type-args by structurally unifying argument types against parameter types. (3) Codegen monomorphizes: one specialized LLVM function per concrete type-arg tuple, emitted by re-running the existing concrete codegen under a `TYPE_PARAM → concrete Type*` substitution environment. `any`-only opaque `T` keeps substitution confined to the type-lowering boundary.

**Tech Stack:** C23, Bison/Flex (`src/parser/parser.y`), LLVM-C backend, Make-driven golden + reject-probe test suites.

## Global Constraints

- **Spec:** `docs/superpowers/specs/2026-07-06-function-generics-tier-a-design.md`. Every task's requirements implicitly include it.
- **Scope:** Ordinary functions only (no generic methods, no generic types). `any`-only constraints. Inference-only (no `Map[int](xs)` call-site syntax). `T` is opaque.
- **Invariant:** Every type parameter must appear in ≥1 parameter type (else reject at declaration).
- **Codegen:** Monomorphization. One instance per distinct concrete type-arg tuple.
- **Grammar gate (mandatory):** `./scripts/grammar-tripwire.sh` must PASS with **exactly 81 shift/reduce + 256 reduce/reduce** before AND after any `parser.y` change. Any delta is stop-the-line — use the **goo-grammar** skill (`.claude/skills/goo-grammar/`) and its conflict-ledger. Never commit a grammar delta without that procedure.
- **Build/test gates:** `make verify` and `make test` must be green. `ccomp` (needed by `make verify`) is in the opam `default` switch — prepend `export PATH="$HOME/.opam/default/bin:$PATH"` to any shell running `make verify`.
- **Branch:** `feat/function-generics-tier-a` (already exists; spec committed at `72efcb7`).
- **Do NOT** use `ast_node_copy` (latent heap-overflow — under-allocates derived structs).
- **Conventional commits**, imperative mood, atomic. Commit trailers per repo convention.
- **No naked returns / return errors, not panic** (project Go-style rules apply to test `.goo` programs).

---

## File Structure

**Front-end**
- `include/ast.h` — add `type_params` field to `FuncDeclNode`.
- `src/ast/ast_constructors.c` — init `type_params = NULL` in `ast_func_decl_new`.
- `src/ast/ast.c` — free `type_params` chain in the `AST_FUNC_DECL` free case.
- `src/parser/parser.y` — new generic `func_decl` productions.

**Type checker**
- `include/types.h` — add `active_type_params` bookkeeping to `TypeChecker`; add `is_generic` + generic metadata to `Variable`.
- `src/types/types.c` — `type_param_new`, `type_substitute`, `unify_types` (new); extend `Variable` init.
- `src/types/type_checker.c` — typevar scoping in `declare_function_signature`/`type_check_function_decl`; `type_from_ast` hook; declaration invariants; skip-codegen guard for generic decls.
- `src/types/expression_checker.c` — generic-call inference in `type_check_call_expr`.

**Codegen (monomorphization)**
- `include/codegen.h` — `active_type_subst` env on `CodeGenerator`; instantiation-request registry; declarations for new functions.
- `src/codegen/type_mapping.c` — `TYPE_PARAM` resolution via the active subst env.
- `src/codegen/monomorphize.c` (**new**) — instantiation registry, worklist, mangling, instance stamping.
- `src/codegen/codegen.c` — drive the monomorphization pass after the declaration loop; skip generic template decls in the normal loop.
- `src/codegen/call_codegen.c` — resolve a generic call to its mangled instance symbol.

**Tests**
- `examples/gen_*.goo` + `.expected.txt` — positive goldens (auto-discovered by `scripts/run_golden.sh`).
- `Makefile` — new reject-probe target `generics-reject-probe`, wired into `verify`.

---

## Milestone 1 — Front-end (parse + AST)

Deliverable: `func Id[T any](x T) T { return x }` parses; `bin/goo --emit-ast` shows populated `type_params`; tripwire green. No type-checking yet.

### Task 1: Add `type_params` to the function AST

**Files:**
- Modify: `include/ast.h:287-300` (`FuncDeclNode`)
- Modify: `src/ast/ast_constructors.c:65-83` (`ast_func_decl_new`)
- Modify: `src/ast/ast.c:153` (`AST_FUNC_DECL` free case)

**Interfaces:**
- Produces: `FuncDeclNode.type_params` — a `struct ASTNode*` linked list of `VarDeclNode` (each: `names[]` = type-param names in a group, `type` = the constraint type node, `any` in Tier A). NULL for non-generic functions.

- [ ] **Step 1: Add the field.** In `include/ast.h`, inside `FuncDeclNode` (after `receiver;`), add:
```c
    // Generic type parameters (`func F[T, U any](...)`). NULL for ordinary
    // functions. Linked list of VarDeclNode: names[] = a type-param group's
    // names, type = the constraint (Tier A: always `any`). Owned by this node.
    struct ASTNode* type_params;
```

- [ ] **Step 2: Init in constructor.** In `ast_func_decl_new` (`ast_constructors.c`), after `node->receiver = NULL;`, add:
```c
    node->type_params = NULL;
```

- [ ] **Step 3: Free the chain.** Read `src/ast/ast.c:153-175` (the `AST_FUNC_DECL` case). Inside it, alongside the existing frees, add before the node is freed:
```c
            ast_node_free_list(func->type_params);
```
(Use whatever list-free helper the surrounding cases use — if the file frees `func->params` via a helper, mirror it; if it frees via a manual `while (n) { next=n->next; ast_node_free(n); n=next; }` loop, mirror that exactly for `type_params`.)

- [ ] **Step 4: Build.** Run: `make bin/goo 2>&1 | tail -5`
Expected: compiles cleanly (pre-existing warnings about `parser.tab.c` recipe + `write_file` unused are OK).

- [ ] **Step 5: Commit.**
```bash
git add include/ast.h src/ast/ast_constructors.c src/ast/ast.c
git commit -m "feat(ast): add type_params field to FuncDeclNode"
```

### Task 2: Grammar — parse the type-parameter clause

**Files:**
- Modify: `src/parser/parser.y:400-442` (`func_decl` — the four non-attribute forms)
- Test: `examples/gen_parse_probe.goo` (throwaway parse check via `--emit-ast`)

**Interfaces:**
- Consumes: `FuncDeclNode.type_params` (Task 1); `func_params` grammar nonterminal (existing, `parser.y`); `reinterpret_grouped_names` (existing helper).
- Produces: parser populates `func->type_params` for `FUNC identifier LBRACKET func_params RBRACKET ...` forms.

**Approach:** Reuse `func_params` for the bracketed list — `[T, U any]` has the same shape as a parameter group `(T, U any)`, so `reinterpret_grouped_names` gives the grouped-names semantics for free, and each group becomes a `VarDeclNode`. Add four new alternatives mirroring the existing `LPAREN`-only, `LPAREN func_params RPAREN`, `LPAREN RPAREN func_result`, and `LPAREN func_params RPAREN func_result` forms, each prefixed with `LBRACKET func_params RBRACKET` between the identifier and the first `LPAREN`.

- [ ] **Step 1: Snapshot the baseline conflict counts.**
Run: `./scripts/grammar-tripwire.sh`
Expected: PASS — 81 shift/reduce, 256 reduce/reduce. **Record this. If it is not exactly 81/256, STOP and reconcile before touching the grammar.**

- [ ] **Step 2: Write the failing parse test.** Create `examples/gen_parse_probe.goo`:
```go
package main

func Id[T any](x T) T { return x }

func Map[T, U any](s []T, f func(T) U) []U {
	return nil
}

func main() {}
```

- [ ] **Step 3: Verify it fails to parse today.**
Run: `./bin/goo --emit-ast examples/gen_parse_probe.goo 2>&1 | head -3`
Expected: `Parse error at examples/gen_parse_probe.goo:3:...: syntax error` (dies at the `[`).

- [ ] **Step 4: Add the generic productions.** In `src/parser/parser.y`, add these four alternatives to `func_decl` immediately after the existing `FUNC identifier LPAREN func_params RPAREN func_result block` form (after line 442):
```c
    | FUNC identifier LBRACKET func_params RBRACKET LPAREN RPAREN block {
        IdentifierNode* ident = (IdentifierNode*)$2;
        FuncDeclNode* func = ast_func_decl_new(ident->name, ident->base.pos);
        reinterpret_grouped_names($4);
        func->type_params = $4;
        func->body = $8;
        func->params = NULL;
        func->return_type = NULL;
        ast_node_free($2);
        $$ = (ASTNode*)func;
    }
    | FUNC identifier LBRACKET func_params RBRACKET LPAREN func_params RPAREN block {
        IdentifierNode* ident = (IdentifierNode*)$2;
        FuncDeclNode* func = ast_func_decl_new(ident->name, ident->base.pos);
        reinterpret_grouped_names($4);
        func->type_params = $4;
        reinterpret_grouped_names($7);
        func->params = $7;
        func->body = $9;
        func->return_type = NULL;
        ast_node_free($2);
        $$ = (ASTNode*)func;
    }
    | FUNC identifier LBRACKET func_params RBRACKET LPAREN RPAREN func_result block {
        IdentifierNode* ident = (IdentifierNode*)$2;
        FuncDeclNode* func = ast_func_decl_new(ident->name, ident->base.pos);
        reinterpret_grouped_names($4);
        func->type_params = $4;
        func->body = $9;
        func->params = NULL;
        func->return_type = $8;
        ast_node_free($2);
        $$ = (ASTNode*)func;
    }
    | FUNC identifier LBRACKET func_params RBRACKET LPAREN func_params RPAREN func_result block {
        IdentifierNode* ident = (IdentifierNode*)$2;
        FuncDeclNode* func = ast_func_decl_new(ident->name, ident->base.pos);
        reinterpret_grouped_names($4);
        func->type_params = $4;
        reinterpret_grouped_names($7);
        func->params = $7;
        func->body = $10;
        func->return_type = $9;
        ast_node_free($2);
        $$ = (ASTNode*)func;
    }
```

- [ ] **Step 5: Re-run the tripwire (the gate).**
Run: `./scripts/grammar-tripwire.sh`
Expected: PASS — **still exactly 81/256**. If the counts changed, STOP: invoke the goo-grammar skill and follow its conflict-ledger procedure before proceeding. Do not "fix" by editing unrelated rules.

- [ ] **Step 6: Rebuild and verify the probe parses.**
Run: `make bin/goo 2>&1 | tail -3 && ./bin/goo --emit-ast examples/gen_parse_probe.goo 2>&1 | grep -iE "type_param|Id|Map|error" | head`
Expected: no parse error; AST dump reaches `main`. (Exact `--emit-ast` format varies — the pass criterion is "no syntax error" and the file compiles far enough to dump.)

- [ ] **Step 7: Confirm no regression on existing goldens' parsing.**
Run: `export PATH="$HOME/.opam/default/bin:$PATH"; make test-golden 2>&1 | tail -3`
Expected: `--- golden: N passed, 0 failed ---` (N unchanged from before this task).

- [ ] **Step 8: Commit.**
```bash
git add src/parser/parser.y examples/gen_parse_probe.goo
git commit -m "feat(parser): parse type-parameter clause on func declarations"
```

---

## Milestone 2 — Type checker (scoping, invariants, inference)

Deliverable: generic decls type-check abstractly; the four reject cases fire; a generic call resolves its concrete result type. Codegen of generic templates is skipped (guarded) so nothing crashes.

### Task 3: Scope type parameters so `T` resolves as a type

**Files:**
- Modify: `include/types.h` (`TypeChecker` struct — add active-type-param stack)
- Modify: `src/types/types.c` (add `type_param_new`)
- Modify: `src/types/type_checker.c:638-713` (`declare_function_signature`), `:715+` (`type_check_function_decl`), `:2470-2478` + `:2516-2523` (`type_from_ast` hooks)

**Interfaces:**
- Produces:
  - `Type* type_param_new(const char* name, int index);` — a `TYPE_PARAM` `Type*` with `data.type_param = {name, index, constraint=NULL}`.
  - `TypeChecker.active_type_params` — a small array/stack of `Type*` (TYPE_PARAM) with a count; consulted by `type_from_ast`.
  - Helpers `type_checker_push_type_param(TypeChecker*, Type*)`, `type_checker_pop_type_params(TypeChecker*, size_t to_count)`, `Type* type_checker_lookup_type_param(TypeChecker*, const char* name)`.

- [ ] **Step 1: Write the failing test.** Create `examples/gen_id_probe.goo` (this must eventually compile+run; for now it must at least type-check the signature+body without "Unknown type 'T'"):
```go
package main

import "fmt"

func Id[T any](x T) T { return x }

func main() {
	fmt.Println(Id(7))
}
```

- [ ] **Step 2: Verify current failure.**
Run: `./bin/goo examples/gen_id_probe.goo -o build/gen_id 2>&1 | head -3`
Expected: a type error mentioning `Unknown type 'T'` (the body/signature can't resolve `T`).

- [ ] **Step 3: Add `type_param_new`.** In `src/types/types.c`, near the other `type_*` constructors:
```c
Type* type_param_new(const char* name, int index) {
    Type* t = type_new(TYPE_PARAM);
    if (!t) return NULL;
    t->name = str_dup(name);
    t->data.type_param.name = t->name;   // alias; freed once via t->name
    t->data.type_param.index = index;
    t->data.type_param.constraint = NULL; // Tier A: `any`, represented as NULL
    return t;
}
```
Declare it in `include/types.h` near the other constructors.

- [ ] **Step 4: Add the active-type-param stack to `TypeChecker`.** In `include/types.h`, inside `struct TypeChecker`, add:
```c
    // Active generic type parameters, innermost function's params on top.
    // Consulted by type_from_ast so `T` resolves to its TYPE_PARAM Type
    // instead of "Unknown type 'T'". Bounded; Tier A functions are small.
    Type* active_type_params[32];
    size_t active_type_param_count;
```
Zero-init it wherever the checker is constructed (`type_checker_new`/`type_checker_init` — grep for where other fields are zeroed).

- [ ] **Step 5: Add push/pop/lookup helpers.** In `src/types/type_checker.c`:
```c
void type_checker_push_type_param(TypeChecker* checker, Type* tp) {
    if (checker->active_type_param_count < 32)
        checker->active_type_params[checker->active_type_param_count++] = tp;
}
void type_checker_pop_type_params(TypeChecker* checker, size_t to_count) {
    checker->active_type_param_count = to_count;
}
Type* type_checker_lookup_type_param(TypeChecker* checker, const char* name) {
    for (size_t i = checker->active_type_param_count; i-- > 0; ) {
        Type* tp = checker->active_type_params[i];
        if (tp && tp->data.type_param.name &&
            strcmp(tp->data.type_param.name, name) == 0)
            return tp;
    }
    return NULL;
}
```
Declare the three in `include/types.h`.

- [ ] **Step 6: Hook `type_from_ast`.** In `src/types/type_checker.c`, in the `AST_IDENTIFIER` branch, insert BEFORE the `type_error(... "Unknown type '%s'" ...)` at line 2477:
```c
            Type* tp_ident = type_checker_lookup_type_param(checker, ident->name);
            if (tp_ident) return tp_ident;
```
Insert the analogous block in the `AST_BASIC_TYPE` branch before line 2523, using `basic->name`.

- [ ] **Step 7: Push type params during signature + body checking.** In `declare_function_signature` (near the top, before param types are resolved at `type_checker.c:663`), and symmetrically in `type_check_function_decl` (before the body scope, around `:729`), add:
```c
    size_t saved_tp = checker->active_type_param_count;
    if (func->type_params) {
        int idx = 0;
        for (ASTNode* tp = func->type_params; tp; tp = tp->next) {
            VarDeclNode* g = (VarDeclNode*)tp;
            for (size_t i = 0; i < g->name_count; i++)
                type_checker_push_type_param(checker,
                    type_param_new(g->names[i], idx++));
        }
    }
```
and restore at every return path of those functions:
```c
    type_checker_pop_type_params(checker, saved_tp);
```
(In `declare_function_signature`, restore before each `return`. In `type_check_function_decl`, restore right before the existing `scope_pop(checker); return result;`.)

- [ ] **Step 8: Build and verify `T` now resolves.**
Run: `make bin/goo 2>&1 | tail -3 && ./bin/goo examples/gen_id_probe.goo -o build/gen_id 2>&1 | head -5`
Expected: **no** "Unknown type 'T'". (It may now fail later in codegen — that's fine; Task 5 adds the guard, and full run comes in M3. The pass criterion for this task: the `Unknown type 'T'` error is gone.)

- [ ] **Step 9: Commit.**
```bash
git add include/types.h src/types/types.c src/types/type_checker.c examples/gen_id_probe.goo
git commit -m "feat(types): scope generic type params so T resolves in signatures/bodies"
```

### Task 4: Declaration invariants + generic-template metadata + codegen skip guard

**Files:**
- Modify: `include/types.h` (`Variable` — add `is_generic`, `generic_decl`, type-param count)
- Modify: `src/types/types.c` (`variable_new` init the new fields)
- Modify: `src/types/type_checker.c` (`declare_function_signature` / `type_check_function_decl` — invariants + mark generic)
- Modify: `src/codegen/codegen.c:340+` (`codegen_generate_declaration`) — skip generic templates
- Test: `Makefile` new `generics-reject-probe`

**Interfaces:**
- Produces: `Variable.is_generic` (int), `Variable.generic_decl` (`struct ASTNode*` back-ref to the `FuncDeclNode`), `Variable.type_param_count` (size_t). Set on a generic function's scope Variable.

- [ ] **Step 1: Write failing reject-probes.** Add target `generics-reject-probe` to `Makefile` (model on `iface-satisfaction-probe`, ~line 2148). Cases:
```make
generics-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== generics-reject-probe: generic function declaration invariants ==="
	@printf 'package main\nfunc Zero[T any]() T { var z T\n return z }\nfunc main() {}\n' > build/gen_uninferable.goo
	@printf 'package main\nfunc F[T Stringer](x T) T { return x }\nfunc main() {}\n' > build/gen_badconstraint.goo
	@printf 'package main\nfunc Add[T any](x T) T { return x + 1 }\nfunc main() {}\n' > build/gen_opaque_op.goo
	@"$(COMPILER)" build/gen_uninferable.goo -o build/gen_uninferable.out 2>build/gen_uninferable.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "generics-reject-probe: FAIL (un-inferable type param compiled)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/gen_uninferable.err; then echo "generics-reject-probe: FAIL (invalid IR)"; cat build/gen_uninferable.err; exit 1; fi; \
	  if ! grep -qiE "never used in a parameter|cannot be inferred" build/gen_uninferable.err; then echo "generics-reject-probe: FAIL (no un-inferable diagnostic)"; cat build/gen_uninferable.err; exit 1; fi
	@"$(COMPILER)" build/gen_badconstraint.goo -o build/gen_badconstraint.out 2>build/gen_badconstraint.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "generics-reject-probe: FAIL (non-any constraint compiled)"; exit 1; fi; \
	  if ! grep -qiE "only .any. type constraints|constraint" build/gen_badconstraint.err; then echo "generics-reject-probe: FAIL (no constraint diagnostic)"; cat build/gen_badconstraint.err; exit 1; fi
	@"$(COMPILER)" build/gen_opaque_op.goo -o build/gen_opaque_op.out 2>build/gen_opaque_op.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "generics-reject-probe: FAIL (arithmetic on opaque T compiled)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/gen_opaque_op.err; then echo "generics-reject-probe: FAIL (invalid IR)"; cat build/gen_opaque_op.err; exit 1; fi
	@echo "generics-reject-probe: PASS"
```

- [ ] **Step 2: Verify it fails now.**
Run: `make generics-reject-probe 2>&1 | tail -4`
Expected: FAIL (both programs currently compile or crash rather than being cleanly rejected).

- [ ] **Step 3: Add `Variable` fields.** In `include/types.h` (`struct Variable`), add:
```c
    int is_generic;                 // 1 for a generic function template
    struct ASTNode* generic_decl;   // its FuncDeclNode (for monomorphization)
    size_t type_param_count;        // number of type params
```
In `variable_new` (`src/types/types.c`), init: `var->is_generic = 0; var->generic_decl = NULL; var->type_param_count = 0;`.

- [ ] **Step 4: Enforce invariants + mark generic.** In `declare_function_signature`, after the type params are pushed (Task 3 Step 7) and param types resolved, add a helper + checks. First a collector:
```c
// Records into `seen[]` (indexed by type_param index) whether each type param
// appears anywhere in `t`.
static void mark_type_params_used(const Type* t, int* seen, size_t n) {
    if (!t) return;
    switch (t->kind) {
        case TYPE_PARAM:
            if (t->data.type_param.index >= 0 &&
                (size_t)t->data.type_param.index < n)
                seen[t->data.type_param.index] = 1;
            return;
        case TYPE_SLICE:   mark_type_params_used(t->data.slice.element_type, seen, n); return;
        case TYPE_POINTER: mark_type_params_used(t->data.pointer.pointee_type, seen, n); return;
        case TYPE_ARRAY:   mark_type_params_used(t->data.array.element_type, seen, n); return;
        case TYPE_MAP:
            mark_type_params_used(t->data.map.key_type, seen, n);
            mark_type_params_used(t->data.map.value_type, seen, n);
            return;
        case TYPE_FUNCTION:
            for (size_t i = 0; i < t->data.function.param_count; i++)
                mark_type_params_used(t->data.function.param_types[i], seen, n);
            mark_type_params_used(t->data.function.return_type, seen, n);
            return;
        default: return;
    }
}
```
(Confirm the exact `data.map`/`data.array` field names against `include/types.h` before using; adjust if they differ.)

Then, after building the function's `param_types`, and only when `func->type_params != NULL`:
```c
    size_t tpn = checker->active_type_param_count - saved_tp;
    // Constraint must be `any` (Tier A). `any` => constraint node resolves to
    // TYPE_INTERFACE with 0 methods; anything else is rejected.
    for (ASTNode* tp = func->type_params; tp; tp = tp->next) {
        VarDeclNode* g = (VarDeclNode*)tp;
        Type* c = g->type ? type_from_ast(checker, g->type) : NULL;
        if (!c || c->kind != TYPE_INTERFACE || c->data.interface.method_count != 0) {
            type_error(checker, func->base.pos,
                "only `any` type constraints are supported in v1");
            type_checker_pop_type_params(checker, saved_tp);
            return 0;
        }
    }
    // Every type param must appear in a parameter type (inference-only rule).
    int used[32] = {0};
    for (size_t i = 0; i < param_count; i++)
        mark_type_params_used(param_types[i], used, tpn);
    // recover the param-group names for the diagnostic
    { int idx = 0;
      for (ASTNode* tp = func->type_params; tp; tp = tp->next) {
        VarDeclNode* g = (VarDeclNode*)tp;
        for (size_t i = 0; i < g->name_count; i++, idx++) {
            if (!used[idx]) {
                type_error(checker, func->base.pos,
                    "type parameter %s is never used in a parameter; cannot be inferred",
                    g->names[i]);
                type_checker_pop_type_params(checker, saved_tp);
                return 0;
            }
        }
      }
    }
```
And when registering the function's scope `Variable` (the `func_var` at `type_checker.c:702`), set:
```c
    if (func->type_params) {
        func_var->is_generic = 1;
        func_var->generic_decl = (struct ASTNode*)func;
        func_var->type_param_count = tpn;
    }
```

- [ ] **Step 5: Guard codegen against generic templates.** In `src/codegen/codegen.c`, in `codegen_generate_declaration` (line 340+), at the top of the `AST_FUNC_DECL` case, skip templates (they are emitted per-instance by the monomorphizer in M3):
```c
        // A generic function template (type_params != NULL) is never emitted
        // directly — it is monomorphized per concrete instantiation. Skip here.
        if (((FuncDeclNode*)decl)->type_params) return 1;
```

- [ ] **Step 6: Build + pass the reject-probe.**
Run: `make bin/goo 2>&1 | tail -3 && make generics-reject-probe 2>&1 | tail -3`
Expected: `generics-reject-probe: PASS`.

- [ ] **Step 7: Wire the probe into `verify`.** In `Makefile`, add `generics-reject-probe` to the `verify:` prerequisite list (line ~2016), next to `iface-satisfaction-probe`.

- [ ] **Step 8: Commit.**
```bash
git add include/types.h src/types/types.c src/types/type_checker.c src/codegen/codegen.c Makefile
git commit -m "feat(types): enforce generic decl invariants; mark templates; skip in codegen"
```

### Task 5: `type_substitute` and `unify_types`

**Files:**
- Modify: `src/types/types.c` (add both), `include/types.h` (declare)
- Test: `examples/gen_map_probe.goo` (checked via result-type behavior in Task 6; here a compile-smoke)

**Interfaces:**
- Produces:
  - `Type* type_substitute(Type* t, Type** bindings, size_t n);` — returns a new `Type*` with every `TYPE_PARAM` of index `i < n` replaced by `bindings[i]` (structurally, recursing through slice/pointer/array/map/function). `TYPE_PARAM` with no binding → returned as-is.
  - `int unify_types(Type* param, Type* arg, Type** bindings, size_t n);` — structurally matches `param` (may contain `TYPE_PARAM`) against concrete `arg`, writing inferred concrete types into `bindings[index]`. Returns 1 on success, 0 on structural mismatch or a conflicting binding (`bindings[i]` already set to a different type).

- [ ] **Step 1: Write `type_substitute`.** In `src/types/types.c`:
```c
Type* type_substitute(Type* t, Type** bindings, size_t n) {
    if (!t) return NULL;
    switch (t->kind) {
        case TYPE_PARAM: {
            int i = t->data.type_param.index;
            if (i >= 0 && (size_t)i < n && bindings[i]) return bindings[i];
            return t;
        }
        case TYPE_SLICE:
            return type_slice(type_substitute(t->data.slice.element_type, bindings, n));
        case TYPE_POINTER:
            return type_pointer(type_substitute(t->data.pointer.pointee_type, bindings, n));
        case TYPE_FUNCTION: {
            size_t pc = t->data.function.param_count;
            Type** ps = pc ? calloc(pc, sizeof(Type*)) : NULL;
            for (size_t i = 0; i < pc; i++)
                ps[i] = type_substitute(t->data.function.param_types[i], bindings, n);
            Type* r = type_substitute(t->data.function.return_type, bindings, n);
            Type* ft = type_function(ps, pc, r);
            if (ft) ft->data.function.is_variadic = t->data.function.is_variadic;
            return ft;
        }
        default:
            return t; // concrete types are shared unchanged
    }
}
```
(Verify the `TYPE_ARRAY`/`TYPE_MAP` field names and add cases if a test needs them; Tier A goldens use slice/pointer/function/scalar.)

- [ ] **Step 2: Write `unify_types`.** In `src/types/types.c`:
```c
int unify_types(Type* param, Type* arg, Type** bindings, size_t n) {
    if (!param || !arg) return 0;
    if (param->kind == TYPE_PARAM) {
        int i = param->data.type_param.index;
        if (i < 0 || (size_t)i >= n) return 0;
        if (bindings[i]) return type_equals(bindings[i], arg);
        bindings[i] = arg;
        return 1;
    }
    if (param->kind != arg->kind) return 0;
    switch (param->kind) {
        case TYPE_SLICE:
            return unify_types(param->data.slice.element_type,
                               arg->data.slice.element_type, bindings, n);
        case TYPE_POINTER:
            return unify_types(param->data.pointer.pointee_type,
                               arg->data.pointer.pointee_type, bindings, n);
        case TYPE_FUNCTION: {
            if (param->data.function.param_count != arg->data.function.param_count)
                return 0;
            for (size_t i = 0; i < param->data.function.param_count; i++)
                if (!unify_types(param->data.function.param_types[i],
                                 arg->data.function.param_types[i], bindings, n))
                    return 0;
            return unify_types(param->data.function.return_type,
                               arg->data.function.return_type, bindings, n);
        }
        default:
            return type_equals(param, arg);
    }
}
```

- [ ] **Step 3: Declare both** in `include/types.h`.

- [ ] **Step 4: Build.**
Run: `make bin/goo 2>&1 | tail -3`
Expected: clean compile.

- [ ] **Step 5: Commit.**
```bash
git add src/types/types.c include/types.h
git commit -m "feat(types): add type_substitute and structural unify_types for generics"
```

### Task 6: Wire generic-call inference into call checking

**Files:**
- Modify: `src/types/expression_checker.c:2226+` (`type_check_call_expr`), specifically the arity/type-check region (~2700-2900)
- Test: `examples/gen_map_probe.goo`, and a conflict reject-probe case

**Interfaces:**
- Consumes: `Variable.is_generic`, `Variable.type_param_count`, `Variable.type` (the generic `TYPE_FUNCTION` with `TYPE_PARAM`s); `unify_types`, `type_substitute` (Task 5).
- Produces: for a resolved generic call, sets the call node's `node_type` to the substituted return type; records the concrete type-arg tuple for codegen (an accessor `const Type* const* type_check_call_type_args(ASTNode* call, size_t* out_n)` OR a field on the call node — see Step 5).

- [ ] **Step 1: Write the failing test.** Create `examples/gen_map_probe.goo` + `.expected.txt`:
```go
package main

import "fmt"

func Map[T, U any](s []T, f func(T) U) []U {
	out := make([]U, 0, len(s))
	for _, v := range s {
		out = append(out, f(v))
	}
	return out
}

func main() {
	xs := []int{1, 2, 3}
	ys := Map(xs, func(x int) int { return x * 10 })
	fmt.Println(ys[0], ys[1], ys[2])
}
```
`examples/gen_map_probe.expected.txt`:
```
10 20 30
```
And a conflict case for the reject-probe (add to `generics-reject-probe`): a call binding `T` two ways, e.g. a two-param `func Pair[T any](a T, b T) T` called `Pair(1, "x")` → expect `cannot infer T: conflicting types`.

- [ ] **Step 2: Verify failure.**
Run: `./bin/goo examples/gen_map_probe.goo -o build/gen_map 2>&1 | head -5`
Expected: fails (no inference yet — the call type-checks against `TYPE_PARAM` param types and the result type is wrong/unknown).

- [ ] **Step 3: Locate the callee-Variable resolution in `type_check_call_expr`.** Read `src/types/expression_checker.c:2226` through the arity block (~2853). Find where the callee's `func_type`/`Variable` is obtained. Insert generic handling right after the callee Variable is known and BEFORE the normal fixed-arity param/arg compatibility loop.

- [ ] **Step 4: Add the inference block.** Using the callee Variable `callee_var` and its generic `TYPE_FUNCTION` `gsig = callee_var->type`:
```c
    if (callee_var && callee_var->is_generic) {
        size_t n = callee_var->type_param_count;
        Type** bindings = calloc(n ? n : 1, sizeof(Type*));
        size_t argc = /* count call args */;
        size_t pc = gsig->data.function.param_count;
        if (argc != pc) {
            type_error(checker, expr->pos,
                "wrong number of arguments to %s: expected %zu, got %zu",
                callee_name, pc, argc);
            free(bindings); return NULL;
        }
        size_t k = 0;
        for (ASTNode* a = call_args; a; a = a->next, k++) {
            Type* at = type_check_expression(checker, a);
            if (!at) { free(bindings); return NULL; }
            if (!unify_types(gsig->data.function.param_types[k], at, bindings, n)) {
                type_error(checker, expr->pos,
                    "cannot infer type arguments for %s: argument %zu (%s) does not match",
                    callee_name, k + 1, type_to_string(at));
                free(bindings); return NULL;
            }
        }
        for (size_t i = 0; i < n; i++) {
            if (!bindings[i]) {
                type_error(checker, expr->pos,
                    "cannot infer type parameter %zu of %s", i, callee_name);
                free(bindings); return NULL;
            }
        }
        // Record the instantiation for codegen (Step 5) and set result type.
        type_check_record_instantiation(checker, callee_var, bindings, n, expr);
        Type* result = type_substitute(gsig->data.function.return_type, bindings, n);
        expr->node_type = result;
        return result;
    }
```
Note: `unify_types` already returns 0 on a conflicting binding (`Pair(1,"x")`), producing the conflict diagnostic — adjust the message to include "conflicting types" so the reject-probe's grep matches; e.g. detect the already-set case by pre-checking `bindings[i]` and emitting `cannot infer T: conflicting types %s and %s`.

- [ ] **Step 5: Record instantiations.** Add a minimal registry on the checker (or codegen — but the checker runs first). Simplest: attach the concrete type-args to the call node so codegen reads them locally, AND append to a global list the monomorphizer consumes. Add to `include/types.h` (`TypeChecker`): `struct GenericInstantiation* instantiations;` (linked list of `{Variable* fn; Type** args; size_t n;}`), and:
```c
void type_check_record_instantiation(TypeChecker* checker, Variable* fn,
                                     Type** args, size_t n, ASTNode* call_site);
```
Store the args on `call_site->node_type`? No — `node_type` is the result. Instead add a field to `CallExprNode` (`include/ast.h`): `struct Type** type_args; size_t type_arg_count;` set here, so codegen (Task 9/12) reads the concrete args directly off the call node. Also append `{fn, args, n}` to `checker->instantiations` (dedup later in codegen).

- [ ] **Step 6: Build + pass the Map golden (type-check only far enough).**
Run: `make bin/goo 2>&1 | tail -3 && ./bin/goo examples/gen_map_probe.goo -o build/gen_map 2>&1 | head`
Expected: type-checking passes; it may still fail in codegen (monomorphization is M3). Pass criterion: no type errors; failure (if any) is a codegen/`TYPE_PARAM`-lowering error, not a type error.

- [ ] **Step 7: Conflict reject-probe passes.**
Run: `make generics-reject-probe 2>&1 | tail -3`
Expected: PASS (including the new conflict case).

- [ ] **Step 8: Commit.**
```bash
git add src/types/expression_checker.c include/types.h include/ast.h src/ast/*.c Makefile examples/gen_map_probe.goo examples/gen_map_probe.expected.txt
git commit -m "feat(types): infer type args at generic call sites; record instantiations"
```

---

## Milestone 3 — Monomorphization codegen

Deliverable: `gen_id_probe`, `gen_map_probe`, `gen_filter_probe`, a generic-calls-generic case, and a two-instantiation case all compile+run to their expected output; `make verify` green.

### Task 7: Mangling for concrete type-arg tuples

**Files:**
- Create: `src/codegen/monomorphize.c`; declare in `include/codegen.h`
- Modify: `Makefile` (add `monomorphize.o` to the codegen objects list)

**Interfaces:**
- Produces:
  - `char* codegen_type_mangle_token(const Type* t);` — a nameable token for a concrete type: `int`, `string`, `float64`, `ptr_T`, `slice_T`, `func_...`. Caller frees.
  - `char* codegen_mangle_instance(const char* base, Type* const* args, size_t n);` — `Map` + `{int,string}` → `Map__int__string`. Caller frees.

- [ ] **Step 1: Write the failing unit-ish test.** Create `examples/gen_two_inst_probe.goo` + `.expected.txt` (proves distinct instances via distinct mangled names — validated end-to-end in Task 11, but authored now):
```go
package main

import "fmt"

func Id[T any](x T) T { return x }

func main() {
	fmt.Println(Id(7))
	fmt.Println(Id(true))
}
```
`.expected.txt`:
```
7
true
```
(Two instantiations of `Id`: `Id__int` and `Id__bool`.)

- [ ] **Step 2: Create `monomorphize.c` with the manglers.**
```c
#include "codegen.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

char* codegen_type_mangle_token(const Type* t) {
    if (!t) return str_dup("void");
    switch (t->kind) {
        case TYPE_POINTER: {
            char* inner = codegen_type_mangle_token(t->data.pointer.pointee_type);
            char* out = malloc(strlen(inner) + 5);
            sprintf(out, "ptr_%s", inner); free(inner); return out;
        }
        case TYPE_SLICE: {
            char* inner = codegen_type_mangle_token(t->data.slice.element_type);
            char* out = malloc(strlen(inner) + 7);
            sprintf(out, "slice_%s", inner); free(inner); return out;
        }
        default: {
            const char* n = type_receiver_name(t);
            if (!n) n = type_to_string(t); // fallback for scalars
            return str_dup(n ? n : "T");
        }
    }
}

char* codegen_mangle_instance(const char* base, Type* const* args, size_t n) {
    size_t cap = strlen(base) + 1;
    char** toks = calloc(n ? n : 1, sizeof(char*));
    for (size_t i = 0; i < n; i++) { toks[i] = codegen_type_mangle_token(args[i]); cap += strlen(toks[i]) + 2; }
    char* out = malloc(cap); strcpy(out, base);
    for (size_t i = 0; i < n; i++) { strcat(out, "__"); strcat(out, toks[i]); free(toks[i]); }
    free(toks); return out;
}
```
(Confirm `type_receiver_name` returns `int`/`string`/etc. for scalars; if not, use `type_to_string` which yields those names — verify against `src/types/types.c`.)

- [ ] **Step 3: Declare both** in `include/codegen.h`.

- [ ] **Step 4: Add `monomorphize.o` to the Makefile** codegen objects (grep the Makefile for `interface_codegen.o` and add `monomorphize.o` beside it in the same variable).

- [ ] **Step 5: Build.**
Run: `make bin/goo 2>&1 | tail -3`
Expected: clean compile (manglers unused-but-defined is fine; they're called in Task 11).

- [ ] **Step 6: Commit.**
```bash
git add src/codegen/monomorphize.c include/codegen.h Makefile examples/gen_two_inst_probe.goo examples/gen_two_inst_probe.expected.txt
git commit -m "feat(codegen): add type-arg mangling for monomorphized instances"
```

### Task 8: Substitution environment + `TYPE_PARAM` lowering

**Files:**
- Modify: `include/codegen.h` (`CodeGenerator` — add subst env), `src/codegen/type_mapping.c:9` (`codegen_type_to_llvm`)

**Interfaces:**
- Produces:
  - `CodeGenerator.active_subst` (`Type**`) + `CodeGenerator.active_subst_n` (`size_t`) — when set, `TYPE_PARAM` index `i` lowers as `active_subst[i]`.
  - `const Type* codegen_resolve_type(CodeGenerator* codegen, const Type* t);` — returns the concrete type for a `TYPE_PARAM` under the active env (recursing one level); identity otherwise.

- [ ] **Step 1: Add the env fields** to `CodeGenerator` in `include/codegen.h`:
```c
    Type** active_subst;     // TYPE_PARAM index -> concrete Type*, or NULL
    size_t active_subst_n;
```
Zero-init where the generator is constructed.

- [ ] **Step 2: Add `codegen_resolve_type`.** In `src/codegen/type_mapping.c`:
```c
const Type* codegen_resolve_type(CodeGenerator* codegen, const Type* t) {
    if (t && t->kind == TYPE_PARAM && codegen->active_subst) {
        int i = t->data.type_param.index;
        if (i >= 0 && (size_t)i < codegen->active_subst_n && codegen->active_subst[i])
            return codegen->active_subst[i];
    }
    return t;
}
```
Declare in `include/codegen.h`.

- [ ] **Step 3: Handle `TYPE_PARAM` in `codegen_type_to_llvm`.** At the very top of the `switch` (before `case TYPE_VOID`), resolve through the env:
```c
    if (type->kind == TYPE_PARAM) {
        const Type* r = codegen_resolve_type(codegen, type);
        if (r == type) return NULL; // unbound TYPE_PARAM at lowering = internal error
        return codegen_type_to_llvm(codegen, r);
    }
```

- [ ] **Step 4: Build.**
Run: `make bin/goo 2>&1 | tail -3`
Expected: clean compile. Existing concrete programs still lower (env is NULL, so `TYPE_PARAM` never occurs on the non-generic path).

- [ ] **Step 5: Regression check.**
Run: `export PATH="$HOME/.opam/default/bin:$PATH"; make test-golden 2>&1 | tail -2`
Expected: `N passed, 0 failed` (unchanged).

- [ ] **Step 6: Commit.**
```bash
git add include/codegen.h src/codegen/type_mapping.c
git commit -m "feat(codegen): TYPE_PARAM lowering via a substitution environment"
```

### Task 9: Monomorphization worklist + instance stamping

**Files:**
- Modify: `src/codegen/monomorphize.c` (the pass), `src/codegen/codegen.c:326` (call the pass after the decl loop), `include/codegen.h`

**Interfaces:**
- Consumes: `TypeChecker.instantiations` (Task 6), `codegen_generate_function_decl` (`function_codegen.c:768`), `codegen_predeclare_function` (`function_codegen.c:678`), the subst env (Task 8), the manglers (Task 7).
- Produces: `int codegen_monomorphize(CodeGenerator* codegen, TypeChecker* checker);` — emits one specialized function per unique instantiation, transitively.

**Design of stamping:** For an instantiation `(genericVar, args[n])`:
1. Compute `sym = codegen_mangle_instance(genericVar->name, args, n)`. If `LLVMGetNamedFunction(module, sym)` exists, skip (emit-once).
2. Set `codegen->active_subst = args; codegen->active_subst_n = n;`.
3. Temporarily install the type params of `genericVar->generic_decl` into the *checker's* active-type-param stack bound to `args` — but codegen reads `node_type`s already set during checking (which contain `TYPE_PARAM`). Because `codegen_type_to_llvm` now resolves `TYPE_PARAM` via `active_subst`, calling the ordinary `codegen_generate_function_decl` on the template `FuncDeclNode` under a *rename* to `sym` produces the specialized body. Provide the rename by adding an optional `const char* symbol_override` path (see Step 2).
4. Unset the env.

- [ ] **Step 1: Write the end-to-end failing test.**
Run: `./bin/goo examples/gen_id_probe.goo -o build/gen_id 2>&1 | head; echo rc=$?`
Expected: fails at codegen (the template is skipped, no instance emitted, the call to `Id` finds no symbol). This is the red state Task 9+10 turn green.

- [ ] **Step 2: Add a symbol-override entry to function codegen.** Read `codegen_generate_function_decl` (`function_codegen.c:768-915`). Add a sibling entry point that reuses the body-emission but forces the symbol name and a subst env:
```c
int codegen_generate_function_instance(CodeGenerator* codegen, TypeChecker* checker,
                                       FuncDeclNode* tmpl, const char* sym,
                                       Type** args, size_t n) {
    Type** saved = codegen->active_subst; size_t saved_n = codegen->active_subst_n;
    codegen->active_subst = args; codegen->active_subst_n = n;
    // Reuse the existing lowering. The cleanest reuse is to factor the guts of
    // codegen_generate_function_decl to accept an override symbol; if that
    // refactor is too invasive, temporarily set a codegen->symbol_override
    // field consulted at the mangling site (function_codegen.c:782-805) and
    // clear it after.
    codegen->symbol_override = sym;
    int ok = codegen_generate_function_decl(codegen, checker, (ASTNode*)tmpl);
    codegen->symbol_override = NULL;
    codegen->active_subst = saved; codegen->active_subst_n = saved_n;
    return ok;
}
```
Add `const char* symbol_override;` to `CodeGenerator` (NULL by default) and, at `function_codegen.c:804` where `symbol_name` is finalized, prefer it:
```c
    const char* symbol_name = codegen->symbol_override
        ? codegen->symbol_override
        : codegen_package_symbol_name(checker, base_name);
```
IMPORTANT: also bypass the "skip generic template" guard when `symbol_override` is set — the guard from Task 4 Step 5 is in `codegen_generate_declaration`, not `_function_decl`, so `codegen_generate_function_decl` called directly here is not blocked. Confirm this call path does not re-enter the guard.

- [ ] **Step 3: Write the worklist.** In `monomorphize.c`:
```c
int codegen_monomorphize(CodeGenerator* codegen, TypeChecker* checker) {
    // Fixpoint over checker->instantiations. Because a generic body may call
    // another generic, emitting one instance can surface new ones; the call
    // sites inside a template already carry concrete type_args ONLY when the
    // caller is concrete. For generic-calls-generic, the inner call's type_args
    // are expressed in the caller's type params — resolve them through the
    // current subst env at emit time and enqueue. (Tier A test set includes one
    // such case; implement enqueue-on-discovery by scanning emitted call nodes,
    // or — simpler for Tier A — recursively pre-walk each template body under
    // the active subst env collecting nested generic calls before stamping.)
    for (GenericInstantiation* it = checker->instantiations; it; it = it->next) {
        char* sym = codegen_mangle_instance(it->fn->name, it->args, it->n);
        if (!LLVMGetNamedFunction(codegen->module, sym)) {
            codegen_generate_function_instance(codegen, checker,
                (FuncDeclNode*)it->fn->generic_decl, sym, it->args, it->n);
        }
        free(sym);
    }
    return codegen->error_count == 0;
}
```
For the generic-calls-generic case, add a pre-declaration pass so forward references resolve: before stamping bodies, `codegen_predeclare` each instance's prototype under its mangled `sym` (mirror `codegen_predeclare_function`, but with the subst env active so the signature lowers concretely).

- [ ] **Step 4: Call the pass** in `codegen_generate_program` (`codegen.c`), right after the declaration loop (after line 326, before the `goo.global_init` fill at 333):
```c
    if (is_main_pass && !codegen_monomorphize(codegen, checker)) {
        return 0;
    }
```

- [ ] **Step 5: Build.**
Run: `make bin/goo 2>&1 | tail -5`
Expected: clean compile. (End-to-end run still needs Task 10's call rewiring.)

- [ ] **Step 6: Commit.**
```bash
git add src/codegen/monomorphize.c src/codegen/function_codegen.c src/codegen/codegen.c include/codegen.h
git commit -m "feat(codegen): monomorphization worklist + per-instance stamping"
```

### Task 10: Rewire generic call sites to instance symbols

**Files:**
- Modify: `src/codegen/call_codegen.c:189-209` (`codegen_resolve_callee`) and the direct-call emission (~1525)

**Interfaces:**
- Consumes: `CallExprNode.type_args`/`type_arg_count` (Task 6 Step 5), `codegen_mangle_instance` (Task 7), the generic callee `Variable`.

- [ ] **Step 1: Red state.**
Run: `./bin/goo examples/gen_id_probe.goo -o build/gen_id 2>&1; echo rc=$?`
Expected: still fails — the call to `Id` resolves the bare name (no such concrete symbol) rather than `Id__int`.

- [ ] **Step 2: Resolve generic calls to the mangled instance.** In `codegen_resolve_callee` (or at the call node before resolution), when the callee identifier names a generic function and the `CallExprNode` carries `type_args`, build the instance symbol and look that up instead:
```c
    if (call && call->type_arg_count > 0) {
        // callee is generic; call the monomorphized instance
        char* sym = codegen_mangle_instance(id->name, call->type_args, call->type_arg_count);
        LLVMValueRef inst = LLVMGetNamedFunction(codegen->module, sym);
        free(sym);
        if (inst) return inst;
    }
```
Place this so it takes precedence over the bare-name lookup. (The instance exists because `codegen_monomorphize` ran during the same module generation, before bodies that call it are finalized — verify ordering; if a concrete `main` is emitted before the monomorphizer, move the monomorphize call earlier, or predeclare instances in Task 9 Step 3 so the symbol exists at `main`'s emission.)

- [ ] **Step 3: Build + run the walking-skeleton goldens.**
```bash
make bin/goo 2>&1 | tail -3
./bin/goo examples/gen_id_probe.goo -o build/gen_id && ./build/gen_id
./bin/goo examples/gen_two_inst_probe.goo -o build/gen_two && ./build/gen_two
./bin/goo examples/gen_map_probe.goo -o build/gen_map && ./build/gen_map
```
Expected: `7` / `7`+`true` / `10 20 30` respectively.

- [ ] **Step 4: Commit.**
```bash
git add src/codegen/call_codegen.c
git commit -m "feat(codegen): dispatch generic calls to monomorphized instance symbols"
```

### Task 11: Full test set + `filter` + generic-calls-generic + gate

**Files:**
- Create: `examples/gen_filter_probe.goo`(+`.expected.txt`), `examples/gen_nested_probe.goo`(+`.expected.txt`)
- Modify: `Makefile` (ensure `generics-reject-probe` in `verify`; goldens auto-discover)

- [ ] **Step 1: Add `Filter`.** `examples/gen_filter_probe.goo`:
```go
package main

import "fmt"

func Filter[T any](s []T, keep func(T) bool) []T {
	out := make([]T, 0, len(s))
	for _, v := range s {
		if keep(v) {
			out = append(out, v)
		}
	}
	return out
}

func main() {
	xs := []int{1, 2, 3, 4}
	ys := Filter(xs, func(x int) bool { return x%2 == 0 })
	fmt.Println(len(ys), ys[0], ys[1])
}
```
`.expected.txt`: `2 2 4`

- [ ] **Step 2: Add generic-calls-generic.** `examples/gen_nested_probe.goo`:
```go
package main

import "fmt"

func Id[T any](x T) T { return x }

func Twice[T any](x T, f func(T) T) T { return f(Id(x)) }

func main() {
	fmt.Println(Twice(5, func(n int) int { return n + n }))
}
```
`.expected.txt`: `10`
(`Twice[int]` calls `Id[int]` — exercises transitive instance discovery.)

- [ ] **Step 3: Run the goldens.**
Run: `export PATH="$HOME/.opam/default/bin:$PATH"; make test-golden 2>&1 | tail -3`
Expected: all pass, including the 5 new `gen_*` goldens. If `gen_nested_probe` fails to find `Id__int`, revisit Task 9 Step 3 (predeclare instances) / the worklist's transitive discovery.

- [ ] **Step 4: Full gates.**
Run: `export PATH="$HOME/.opam/default/bin:$PATH"; make verify 2>&1 | tail -5 && make test 2>&1 | tail -3`
Expected: `verify: ALL GREEN GATES PASSED`; `make test` all pass; tripwire still 81/256.

- [ ] **Step 5: Commit.**
```bash
git add examples/gen_filter_probe.goo examples/gen_filter_probe.expected.txt examples/gen_nested_probe.goo examples/gen_nested_probe.expected.txt
git commit -m "test(generics): filter, nested generic, and full golden coverage"
```

---

## Self-Review notes (for the executor)

- **Field-name verification:** Before using `t->data.map.*`, `t->data.array.*`, `t->data.interface.method_count`, `g->name_count`/`g->names`, and `CallExprNode` fields, confirm the exact names in `include/types.h` / `include/ast.h`. The plan's snippets assume conventional names; adjust to match.
- **`any` representation:** Task 4 assumes `any` resolves to a `TYPE_INTERFACE` with `method_count == 0` (via `type_checker_any_type`). Confirm at `type_checker.c:2463`. If `any` is a distinct sentinel, adjust the constraint check accordingly.
- **Ordering risk (Task 9/10):** the monomorphizer must run — and instances must at least be *predeclared* — before any concrete body that calls them is finalized. If `main` is emitted in the decl loop (before `codegen_monomorphize`), predeclare instance prototypes up front (Task 9 Step 3) so `LLVMGetNamedFunction` succeeds at the call site, with bodies filled by the monomorphizer afterward (mirrors the existing predeclare/define split).
- **Grammar is the highest risk:** if Task 2 Step 5 shows any tripwire delta, STOP and use the goo-grammar skill. Do not proceed with a changed conflict count.
