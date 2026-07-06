# Function Generics Tier B Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a generic type parameter be bounded by an interface, and allow calling that interface's methods on a `T` value (`func Print[T Stringer](x T) string { return x.String() }`).

**Architecture:** Four localized edits on top of Tier A. Capture the bound interface into `TYPE_PARAM.constraint` at declaration; resolve a method call on a `TYPE_PARAM` receiver against the bound's method set (type-check); enforce that the inferred concrete type satisfies the bound at each call site (reusing `type_interface_satisfied`); and resolve the receiver type through the monomorphization subst env in method-call codegen so `T.M()` dispatches to the concrete `C__M`. No grammar/AST change; reuses interface satisfaction + monomorphization from Tier A.

**Tech Stack:** C23, LLVM-C backend, Make-driven golden + reject-probe suites.

## Global Constraints

- **Spec:** `docs/superpowers/specs/2026-07-06-function-generics-tier-b-design.md`. Every task's requirements implicitly include it.
- **Scope:** interface bounds + method calls on bounded `T` only. **No operators on `T`** (needs type-set constraints — later tier). **No** explicit call-site instantiation. Inference-only + un-inferable-reject from Tier A unchanged.
- **Bound kind:** an interface type (named interface, or `any` = 0-method interface). Non-interface bounds rejected with *"type constraint must be an interface"*.
- **No grammar change** — Tier A's `func_params` reuse already parses `[T Stringer]`. Grammar tripwire stays **82 S/R + 256 R/R** (do not touch `parser.y`; if you somehow do, `./scripts/grammar-tripwire.sh` must stay 82/256).
- **Build/test gates:** `make verify` + `make test` green. `ccomp` (for `make verify`) is in the opam `default` switch — prepend `export PATH="$HOME/.opam/default/bin:$PATH"`. Golden baseline before this work: **286 passed, 0 failed**; `make test` **76/1**. Use sequential `make` (a pre-existing bison parallel-build race breaks `-j`).
- **Branch:** `feat/function-generics-tier-b` (stacked on `feat/function-generics-tier-a`; spec committed `4a5a2fb`).
- **Do NOT** use `ast_node_copy` (latent heap-overflow).
- **Conventional commits**, imperative mood, atomic. Sign commits; if 1Password signing errors, retry once then `git commit --no-gpg-sign`.

---

## File Structure

- `src/types/type_checker.c` — `declare_function_signature`: capture the bound into the type param, relax the decl invariant.
- `src/types/expression_checker.c` — `type_check_selector_expr`: method call on a `TYPE_PARAM` receiver; `type_check_generic_call`: call-site bound satisfaction.
- `src/codegen/call_codegen.c` — method-call receiver resolution through the subst env.
- `examples/genb_*.goo` + `.expected.txt` — positive goldens.
- `Makefile` — `generics-bound-reject-probe`, wired into `verify`.

---

## Milestone 1 — Capture the bound + relax the decl invariant

Deliverable: a bounded generic `func Keep[T Stringer](x T) T { return x }` type-checks (bound stored on the type param); a non-interface bound is rejected; `any` still works. Method calls on `T` still rejected (that's M2).

### Task 1: Capture the interface bound into `TYPE_PARAM.constraint`

**Files:**
- Modify: `src/types/type_checker.c` — `declare_function_signature`, the type-param push loop (~750-759) and the Tier-A constraint invariant (~804-821)
- Test: `Makefile` `generics-bound-reject-probe` (new)

**Interfaces:**
- Produces: every pushed `TYPE_PARAM` now carries its bound in `data.type_param.constraint` (a `TYPE_INTERFACE` `Type*`; `any` is a 0-method interface). Consumed by Tasks 2 (method resolution) and 3 (call-site satisfaction).

- [ ] **Step 1: Write the failing reject-probe.** Add target `generics-bound-reject-probe` to `Makefile` (model on `generics-reject-probe`). First case only for now (more added in M2):
```make
generics-bound-reject-probe: $(COMPILER) $(RUNTIME_LIB)
	@mkdir -p build
	@echo "=== generics-bound-reject-probe: interface-constraint bounds ==="
	@printf 'package main\nfunc F[T int](x T) T { return x }\nfunc main() {}\n' > build/genb_noniface.goo
	@"$(COMPILER)" build/genb_noniface.goo -o build/genb_noniface.out 2>build/genb_noniface.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "generics-bound-reject-probe: FAIL (non-interface bound compiled)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/genb_noniface.err; then echo "generics-bound-reject-probe: FAIL (invalid IR)"; cat build/genb_noniface.err; exit 1; fi; \
	  if ! grep -qiE "constraint must be an interface" build/genb_noniface.err; then echo "generics-bound-reject-probe: FAIL (no constraint diagnostic)"; cat build/genb_noniface.err; exit 1; fi
	@echo "generics-bound-reject-probe: PASS"
```

- [ ] **Step 2: Verify it fails now.**
Run: `make generics-bound-reject-probe 2>&1 | tail -4`
Expected: FAIL — today `[T int]` is rejected by the Tier-A "must be `any`" check with a *different* message (not "constraint must be an interface"), so the diagnostic-grep fails.

- [ ] **Step 3: Capture the bound in the push loop.** In `declare_function_signature`, REPLACE the push loop (currently):
```c
    size_t saved_tp = checker->active_type_param_count;
    if (func->type_params) {
        int idx = 0;
        for (ASTNode* tp = func->type_params; tp; tp = tp->next) {
            VarDeclNode* g = (VarDeclNode*)tp;
            for (size_t i = 0; i < g->name_count; i++)
                type_checker_push_type_param(checker,
                    type_param(g->names[i], idx++, NULL));
        }
    }
```
with:
```c
    size_t saved_tp = checker->active_type_param_count;
    if (func->type_params) {
        int idx = 0;
        for (ASTNode* tp = func->type_params; tp; tp = tp->next) {
            VarDeclNode* g = (VarDeclNode*)tp;
            // Tier B: resolve the bound (constraint) once per group. It must be
            // an interface — a named interface, or `any` (the 0-method
            // interface). A non-interface bound is rejected here.
            Type* bound = g->type ? type_from_ast(checker, g->type) : NULL;
            if (!bound || bound->kind != TYPE_INTERFACE) {
                type_error(checker, func->base.pos,
                           "type constraint must be an interface");
                type_checker_pop_type_params(checker, saved_tp);
                return 0;
            }
            for (size_t i = 0; i < g->name_count; i++)
                type_checker_push_type_param(checker,
                    type_param(g->names[i], idx++, bound));
        }
    }
```

- [ ] **Step 4: Remove the now-redundant Tier-A constraint check.** Read the invariant block (~804-830). It currently has TWO parts: (a) a loop that re-resolves each group's constraint and rejects unless it's a 0-method interface (the "only `any`" check), and (b) the un-inferable check (every type param must appear in a parameter). DELETE part (a) only — the constraint is now validated in the push loop (Step 3). KEEP part (b) verbatim (the `mark_type_params_used` / un-inferable-rejection logic) and the `tpn` computation. Also keep marking the Variable `is_generic`/`generic_decl`/`type_param_count`.

- [ ] **Step 5: Build + reject-probe passes.**
Run: `make bin/goo 2>&1 | tail -3 && make generics-bound-reject-probe 2>&1 | tail -2`
Expected: `generics-bound-reject-probe: PASS`.

- [ ] **Step 6: Regression — `any` + existing generics unaffected.**
Run: `export PATH="$HOME/.opam/default/bin:$PATH"; make generics-reject-probe 2>&1 | tail -2 && make test-golden 2>&1 | tail -2`
Expected: `generics-reject-probe: PASS` (the 4 Tier-A invariants incl. un-inferable/opaque-op still fire); golden `286 passed, 0 failed`.

- [ ] **Step 7: Wire the probe into `verify`.** In `Makefile`, add `generics-bound-reject-probe` to the `verify:` prerequisite list next to `generics-reject-probe`.

- [ ] **Step 8: Commit.**
```bash
git add src/types/type_checker.c Makefile
git commit -m "feat(types): capture interface bound on type params; require interface constraint"
```

---

## Milestone 2 — Method calls on a bounded `T` (end-to-end)

Deliverable: `func Print[T Stringer](x T) string { return x.String() }` compiles, runs, and dispatches to the concrete type's method; a non-satisfying arg and an unknown method both reject.

### Task 2: Method-call type-check on `T` + call-site bound satisfaction

**Files:**
- Modify: `src/types/expression_checker.c` — `type_check_selector_expr` (before the terminal reject ~3578); `type_check_generic_call` (after the all-bindings-set loop ~2305)
- Test: extend `Makefile` `generics-bound-reject-probe`

**Interfaces:**
- Consumes: `TYPE_PARAM.constraint` (Task 1); `type_interface_satisfied` (existing); `unify_types`/`bindings` (Tier A `type_check_generic_call`).
- Produces: `x.M()` on a `TYPE_PARAM` receiver type-checks to the bound method's return type; a call whose inferred type doesn't satisfy the bound is rejected.

- [ ] **Step 1: Write the failing reject-probes.** Append two cases to `generics-bound-reject-probe` in the `Makefile` (before the final `@echo ... PASS`):
```make
	@printf 'package main\ntype Stringer interface { String() string }\ntype Pt struct { x int }\nfunc Show[T Stringer](v T) string { return v.String() }\nfunc main() { _ = Show(Pt{x: 1}) }\n' > build/genb_notsat.goo
	@"$(COMPILER)" build/genb_notsat.goo -o build/genb_notsat.out 2>build/genb_notsat.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "generics-bound-reject-probe: FAIL (non-satisfying arg compiled)"; exit 1; fi; \
	  if ! grep -qiE "does not implement" build/genb_notsat.err; then echo "generics-bound-reject-probe: FAIL (no satisfaction diagnostic)"; cat build/genb_notsat.err; exit 1; fi
	@printf 'package main\ntype Stringer interface { String() string }\ntype Pt struct { x int }\nfunc (p Pt) String() string { return "p" }\nfunc Bad[T Stringer](v T) string { return v.Nope() }\nfunc main() { _ = Bad(Pt{x: 1}) }\n' > build/genb_nomethod.goo
	@"$(COMPILER)" build/genb_nomethod.goo -o build/genb_nomethod.out 2>build/genb_nomethod.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "generics-bound-reject-probe: FAIL (unknown bound method compiled)"; exit 1; fi; \
	  if ! grep -qiE "has no method" build/genb_nomethod.err; then echo "generics-bound-reject-probe: FAIL (no unknown-method diagnostic)"; cat build/genb_nomethod.err; exit 1; fi
	@printf 'package main\ntype Stringer interface { String() string }\nfunc Op[T Stringer](a T, b T) T { return a + b }\nfunc main() {}\n' > build/genb_op.goo
	@"$(COMPILER)" build/genb_op.goo -o build/genb_op.out 2>build/genb_op.err; rc=$$?; \
	  if [ $$rc -eq 0 ]; then echo "generics-bound-reject-probe: FAIL (operator on bounded T compiled)"; exit 1; fi; \
	  if grep -qiE "Module verification failed|LLVM ERROR" build/genb_op.err; then echo "generics-bound-reject-probe: FAIL (invalid IR)"; cat build/genb_op.err; exit 1; fi
```
(The `+` on a bounded `T` must still be rejected — operators need type-set constraints, which are out of Tier B scope. `type_is_numeric(TYPE_PARAM)` is false, so this rejects the same way as an `any`-bound `T`.)

- [ ] **Step 2: Verify they fail now.**
Run: `make generics-bound-reject-probe 2>&1 | tail -5`
Expected: FAIL — today `v.String()` on a `TYPE_PARAM` hits *"Selector on non-struct, non-package type"* (not "has no method"), and the non-satisfying arg isn't checked at all (compiles).

- [ ] **Step 3: Add the `TYPE_PARAM` selector branch.** In `type_check_selector_expr` (`expression_checker.c`), immediately BEFORE the terminal `type_error(checker, expr->pos, "Selector on non-struct, non-package type");` (~3578), insert:
```c
    // Function generics Tier B: a method call on a bounded type parameter.
    // `x.M()` where x : TYPE_PARAM resolves M against the bound interface's
    // method set (the checker sees the abstract T; monomorphization later
    // dispatches to the concrete type's M). An `any` (0-method) bound has no
    // methods, so an attempted method call correctly reaches the reject below.
    if (expr_type->kind == TYPE_PARAM &&
        expr_type->data.type_param.constraint &&
        expr_type->data.type_param.constraint->kind == TYPE_INTERFACE) {
        Type* bound = expr_type->data.type_param.constraint;
        for (InterfaceMethod* im = bound->data.interface.methods; im; im = im->next) {
            if (im->name && strcmp(im->name, selector->selector) == 0) {
                expr->node_type = im->type;
                return im->type;
            }
        }
        type_error(checker, expr->pos,
                   "type parameter %s (constraint %s) has no method '%s'",
                   expr_type->data.type_param.name ? expr_type->data.type_param.name : "T",
                   bound->data.interface.name ? bound->data.interface.name : "interface",
                   selector->selector);
        return NULL;
    }
```

- [ ] **Step 4: Add the constraint-lookup helper + call-site satisfaction.** In `expression_checker.c`, add this file-scope helper ABOVE `type_check_generic_call`:
```c
// Tier B: find the bound (constraint interface) for type-param index `idx` by
// locating a TYPE_PARAM with that index anywhere in a generic signature's
// param types. Every type param appears in a parameter (Tier A invariant), so
// this finds it. Returns the constraint Type* (a TYPE_INTERFACE), or NULL.
static Type* generic_param_constraint(Type* t, int idx) {
    if (!t) return NULL;
    switch (t->kind) {
        case TYPE_PARAM:
            return t->data.type_param.index == idx ? t->data.type_param.constraint : NULL;
        case TYPE_SLICE:   return generic_param_constraint(t->data.slice.element_type, idx);
        case TYPE_POINTER: return generic_param_constraint(t->data.pointer.pointee_type, idx);
        case TYPE_FUNCTION: {
            for (size_t i = 0; i < t->data.function.param_count; i++) {
                Type* c = generic_param_constraint(t->data.function.param_types[i], idx);
                if (c) return c;
            }
            return generic_param_constraint(t->data.function.return_type, idx);
        }
        default: return NULL;
    }
}
```
Then in `type_check_generic_call`, immediately AFTER the "all bindings set" loop (the `for (size_t i = 0; i < n; i++) { if (!bindings[i]) ... }` block, ~2305) and BEFORE the instantiation recording (~2307), insert:
```c
    // Tier B: enforce interface-constraint bounds — each inferred concrete type
    // must satisfy its type param's bound. `any` / 0-method bounds are
    // satisfied by everything, so skip them.
    for (size_t i = 0; i < n; i++) {
        Type* bound = NULL;
        for (size_t p = 0; p < pc && !bound; p++)
            bound = generic_param_constraint(gsig->data.function.param_types[p], (int)i);
        if (bound && bound->kind == TYPE_INTERFACE &&
            bound->data.interface.method_count > 0) {
            const char* method = NULL; const char* reason = NULL;
            if (!type_interface_satisfied(checker, bound, bindings[i], &method, &reason)) {
                const char* cn = type_receiver_name(bindings[i]);
                type_error(checker, expr->pos,
                    "%s does not implement %s (%s method %s)",
                    cn ? cn : type_to_string(bindings[i]),
                    bound->data.interface.name ? bound->data.interface.name : "interface",
                    reason ? reason : "missing", method ? method : "?");
                free(bindings);
                return NULL;
            }
        }
    }
```

- [ ] **Step 5: Build + reject-probes pass.**
Run: `make bin/goo 2>&1 | tail -3 && make generics-bound-reject-probe 2>&1 | tail -2`
Expected: `generics-bound-reject-probe: PASS` (all 3 cases: non-interface bound, non-satisfying arg, unknown bound method).

- [ ] **Step 6: Regression.**
Run: `export PATH="$HOME/.opam/default/bin:$PATH"; make test-golden 2>&1 | tail -2 && make generics-reject-probe 2>&1 | tail -2`
Expected: golden `286 passed, 0 failed`; `generics-reject-probe: PASS`. (A positive `Print[T Stringer]` program now type-checks but may still fail at CODEGEN — Task 3 — because a `T` method call isn't yet resolved through the subst env. That's expected here.)

- [ ] **Step 7: Commit.**
```bash
git add src/types/expression_checker.c Makefile
git commit -m "feat(types): resolve method calls on bounded T; enforce bound satisfaction at call sites"
```

### Task 3: Method-call codegen through the subst env + end-to-end goldens

**Files:**
- Modify: `src/codegen/call_codegen.c` — method-call receiver resolution (~1174)
- Create: `examples/genb_print_probe.goo`, `examples/genb_embed_probe.goo`, `examples/genb_two_probe.goo` (+ `.expected.txt`)

**Interfaces:**
- Consumes: `codegen_resolve_type` (Tier A, `type_mapping.c`); `codegen->active_subst`; the Task 2 type-check.

- [ ] **Step 1: Write the failing golden.** Create `examples/genb_print_probe.goo`:
```go
package main

import "fmt"

type Stringer interface {
	Label() int
}

type Node struct {
	id int
}

func (n Node) Label() int { return n.id + 100 }

func Show[T Stringer](x T) int {
	return x.Label()
}

func main() {
	fmt.Println(Show(Node{id: 7}))
}
```
Do NOT create `examples/genb_print_probe.expected.txt` yet (codegen isn't wired — adding it would make `test-golden` red).

- [ ] **Step 2: Verify it fails at codegen today.**
Run: `./bin/goo examples/genb_print_probe.goo -o build/genb_print 2>&1 | head -3`
Expected: it type-checks (Task 2) but codegen fails — `x.Label()` resolves the receiver name to `"T"` and looks up the never-emitted `T__Label` → an "Undefined identifier"/method-not-found style failure. (Confirms the codegen gap.)

- [ ] **Step 3: Resolve the receiver through the subst env.** In `src/codegen/call_codegen.c`, in the method-call block, immediately BEFORE:
```c
        const char* tn = type_receiver_name(recv_type);
```
(~line 1174) insert:
```c
        // Function generics Tier B: if the receiver's static type is a type
        // parameter, resolve it through the active monomorphization subst env
        // so `T.M()` dispatches to the concrete `C__M` (not the never-emitted
        // `T__M`). Identity on the non-generic path (active_subst is NULL).
        recv_type = (Type*)codegen_resolve_type(codegen, recv_type);
```
(The cast is because `codegen_resolve_type` returns `const Type*` while `recv_type` is `Type*`; the value is a shared, non-owned `Type*`.)

- [ ] **Step 4: Build + the print golden runs.**
Run: `make bin/goo 2>&1 | tail -3 && ./bin/goo examples/genb_print_probe.goo -o build/genb_print && ./build/genb_print`
Expected: `107`.
Then write `examples/genb_print_probe.expected.txt`:
```
107
```

- [ ] **Step 5: Add the embedding golden.** Create `examples/genb_embed_probe.goo` (the bound method is promoted through an embedded struct — proves dispatch after substitution handles promoted methods):
```go
package main

import "fmt"

type Labeler interface {
	Label() int
}

type Base struct {
	n int
}

func (b Base) Label() int { return b.n * 2 }

type Wrap struct {
	Base
}

func Show[T Labeler](x T) int { return x.Label() }

func main() {
	fmt.Println(Show(Wrap{Base: Base{n: 21}}))
}
```
Run: `./bin/goo examples/genb_embed_probe.goo -o build/genb_embed && ./build/genb_embed`
Expected: `42`. If it fails because the direct `Wrap__Label` lookup misses the promoted method, extend the codegen method resolution to fall back to the embedding-resolve path (the interface/method codegen already uses `embedding_resolve`); note this in the report. Write `examples/genb_embed_probe.expected.txt` = `42`.

- [ ] **Step 6: Add the two-concrete-types golden.** Create `examples/genb_two_probe.goo` (one bounded generic, two satisfying concrete types → two instances each dispatching correctly):
```go
package main

import "fmt"

type Labeler interface {
	Label() int
}

type A struct{ x int }
func (a A) Label() int { return a.x + 1 }

type B struct{ y int }
func (b B) Label() int { return b.y * 10 }

func Show[T Labeler](v T) int { return v.Label() }

func main() {
	fmt.Println(Show(A{x: 5}))
	fmt.Println(Show(B{y: 5}))
}
```
Run it → expect `6` then `50`. Write `examples/genb_two_probe.expected.txt`:
```
6
50
```

- [ ] **Step 7: Full gates.**
Run: `export PATH="$HOME/.opam/default/bin:$PATH"; make test-golden 2>&1 | tail -3 && make verify 2>&1 | tail -4 && make test 2>&1 | tail -3`
Expected: golden `289 passed, 0 failed` (286 + 3 new); `verify: ALL GREEN GATES PASSED`; `make test` 76/1. (Grammar untouched → tripwire still 82/256.)

- [ ] **Step 8: Commit.**
```bash
git add src/codegen/call_codegen.c examples/genb_print_probe.goo examples/genb_print_probe.expected.txt examples/genb_embed_probe.goo examples/genb_embed_probe.expected.txt examples/genb_two_probe.goo examples/genb_two_probe.expected.txt
git commit -m "feat(codegen): dispatch bounded-T method calls to the concrete instance method"
```

---

## Self-Review notes (for the executor)

- **Field-name verification:** before using `data.type_param.constraint`, `data.interface.methods`/`method_count`, `data.slice.element_type`, `data.pointer.pointee_type`, `data.function.*`, confirm each in `include/types.h` (all were used correctly in Tier A, so they should match).
- **Value vs pointer receiver on a bounded T:** the M2 goldens use value receivers. If a bound method has a pointer receiver, the concrete-type satisfaction check (Task 2, `type_interface_satisfied`, which is receiver-kind-aware after this session's soundness fix) governs acceptance, and the existing method-call codegen auto-addresses an addressable value. Not exercised by the goldens; note if you probe it.
- **The embedding golden (Task 3 Step 5) is the main risk probe** — a promoted bound method must still dispatch after substitution. If the direct mangled lookup misses it, the fallback is the `embedding_resolve` path the interface codegen already uses.
- **No grammar change** — if you find yourself editing `parser.y`, stop: the plan requires none, and the tripwire must stay 82/256.
