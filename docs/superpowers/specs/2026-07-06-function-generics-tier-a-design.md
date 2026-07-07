# Function Generics — Tier A (Walking Skeleton) — Design

**Date:** 2026-07-06
**Status:** Approved design, pre-implementation
**Scope:** Generics for ordinary (non-method) functions, `any`-only constraints,
inference-only instantiation, monomorphized codegen. This is the first of a planned
tier sequence; Tiers B/C get their own specs.

## Context

Goo advertises generics (task-master "Task #22 done": concepts, HKTs, constraint
inference) but **no generic function can be written today**. Verified ground truth
(three independent code audits, 2026-07-06):

- **Parser** — `func_decl` (`src/parser/parser.y:400`) has no type-parameter clause;
  `[` after a function name is a syntax error. `type_param_list` grammar exists but is
  wired **only** to `concept` declarations (`parser.y:958`). Call-site `Map[int](xs)`
  parses as subscript-then-call; `Map[a,b](xs)` is a hard parse error.
- **AST** — `FuncDeclNode` (`include/ast.h:287-300`) has **no** `type_params` field
  (only `ConceptDeclNode` does, at `ast.h:362`).
- **Type checker** — `TYPE_PARAM` kind exists (`include/types.h`, `data.type_param
  {name,index,constraint}`) but is an empty slot. A bare `T` used as a type hits
  `"Unknown type 'T'"` (`type_checker.c:2500`). No typevar scoping, no inference wired
  to calls, and `substitute_type_variables` (`constraint_inference.c:619`) is a stub
  that returns its input unchanged.
- **Task #22 machinery is dead** — `concept_generics.c`, `higher_kinded_types.c`,
  `advanced_constraint_inference.c`, etc. compile and link but are **never invoked** by
  the real pipeline (`type_checker_init_enhanced_interfaces` has zero callers). It is a
  parallel struct hierarchy that does not connect to the real `Type` system, with core
  operations stubbed. **Not reusable** for function generics.
- **Codegen is well-shaped** — the backend is declare-then-define + idempotent
  get-or-create (`codegen.c:251`, `function_codegen.c:678/768/878`), exactly a
  monomorphizer's discipline. Slices `{ptr,len,cap}` (`type_mapping.c:39`) and closures
  `{fn,env}` (fat pointers) already lower for concrete types. Mangling
  (`type_method_mangled_name`, `types.c:792`) is a simple, extensible string builder.
  `TYPE_PARAM` is absent from the `codegen_type_to_llvm` switch → falls to
  `default: return NULL` (a hard error, not a crash).

**Conclusion:** function generics is a greenfield front-half feature (parser → AST →
type checker → a monomorphization codegen pass) with a friendly backend. The intended
outcome of Tier A is a working end-to-end pipeline proving the whole path on the
canonical `Map/Filter/Id` shapes.

## Decisions (locked)

| Axis | Decision |
|---|---|
| Ambition | **Tier A walking skeleton** — functions only; generic *types* deferred. |
| Constraints | **`any` only** — `T` is opaque (no methods/operators/indexing on a `T` value). |
| Instantiation | **Inferred from argument types only**; no `Map[int](xs)` call-site syntax. |
| Un-inferable type param | **Reject at declaration** — every type param must appear in ≥1 parameter type. |
| Codegen | **Monomorphization** — one specialized function per concrete type-arg tuple. |

## 1. Surface contract

**Compiles (Go-compatible syntax):**
```go
func Id[T any](x T) T { return x }
func Map[T, U any](s []T, f func(T) U) []U { ... }
func Filter[T any](s []T, keep func(T) bool) []T { ... }

ys := Map(xs, func(x int) string { return itoa(x) })  // T=int, U=string inferred
```
Type-param list forms accepted: `[T any]`, `[T, U any]`, `[T any, U any]`.

`T` is **opaque**: a value of type `T` may only be assigned/moved, stored in `[]T`,
passed where `T` or `any` is expected, returned, and zero-valued (`var z T`). No
operators, method calls, indexing, or field access on a `T` value (those need
constraints — Tier B).

**Rejected with clean diagnostics (non-zero exit, no LLVM-verifier crash):**
- Type param unused in parameters → *"type parameter T is never used in a parameter;
  cannot be inferred"* (at the declaration).
- Constraint other than `any` → *"only `any` type constraints are supported in v1"*.
- Conflicting inference → *"cannot infer T: conflicting types int and string"*.
- Concreteness-requiring op on opaque `T` (`x + 1`, `x.M()`) → ordinary type error.

## 2. Front-end (grammar + AST)

- **Grammar** (`parser.y`): add an optional `[ type_param_list ]` to `func_decl`,
  between the function name and `(`. The list mirrors a parameter group (`ident-list
  constraint-type`), reusing the existing param-group shape. It sits in *declaration*
  position, away from expression-context `index_expr`, so conflict risk is low.
  Generic **methods are out of scope** — the receiver grammar is untouched.
  - **Grammar gate (mandatory):** `./scripts/grammar-tripwire.sh` must PASS
    (81 S/R + 256 R/R exact) **before and after**. Any delta is stop-the-line via the
    goo-grammar skill's conflict-ledger. Use the goo-grammar skill for this change.
- **AST** (`include/ast.h`, `src/ast/`): add `struct ASTNode* type_params;` to
  `FuncDeclNode`. Each type param = name + constraint-type node (`any` in Tier A; slot
  future-proofs Tier B bounds). Update the `FuncDeclNode` constructor (init NULL) and
  the `ast.c` free-path to free the chain. **Do not** route this through
  `ast_node_copy` — it is a known latent heap-overflow (project memory).

## 3. Type checker

**a. Typevar scoping.** For a generic `func` decl, create one `TYPE_PARAM` `Type*` per
type param (`{name, index, constraint}`), and push them onto a new
`checker->active_type_params` stack *before* resolving parameter/return types. Extend
`type_from_ast`'s identifier path to consult active type params before emitting
`"Unknown type 'T'"`. This single hook makes `T` resolve as a type in the signature and
body. Pop on exit.

**b. Generic signature + registration.** The function's `TYPE_FUNCTION` carries
`TYPE_PARAM`s inside its param/return types (e.g. `[]T` = `TYPE_SLICE` of `TYPE_PARAM`).
Mark the checker `Variable` `is_generic`, and stash the type-param list + a
back-reference to the `FuncDeclNode` (needed for monomorphization).

**c. Declaration-time invariants (Tier A):**
- Collect `TYPE_PARAM`s referenced across all parameter types; every declared type
  param must appear ≥1, else reject (un-inferable rule).
- Each constraint must be `any`, else reject.

**d. Abstract body check (once).** Type-check the body with `T` opaque — assignable only
to itself and to `any`; `var z T` allowed. Catches instantiation-independent errors
early (Go does this).

**e. Call-site inference + substitution** (new reusable primitives; replace the dead
`substitute_type_variables` stub):
- **Structural unification** of each parameter's declared type against the argument's
  concrete type: `T`↔`int` binds `T=int`; `[]T`↔`[]int` recurses element;
  `func(T) U`↔`func(int) string` recurses params+return; pointer/map recurse. Conflicting
  bindings → clean error.
- **`type_substitute(Type*, bindings)`** builds the concrete signature; the call's result
  type is the substituted return (`[]U` → `[]string`).
- **Record an instantiation request.** From concrete call sites, type-args are concrete →
  recorded directly. From inside a generic body, the callee's type-args are expressed in
  the caller's type params (`A[T]` calling `B(x)` ⇒ `B[T]`) → recorded symbolically,
  resolved during monomorphization.

## 4. Monomorphization codegen

**a. Instance worklist (transitive closure).** Seed with requests from concrete call
sites. Process each `(genericFunc, concreteTypeArgs)`; while stamping, symbolic
generic-calls inside its body get the caller's bindings substituted → new concrete
requests enqueued. Dedup by mangled name → emit-once (matches existing get-or-create).

**b. Mangling.** Extend `type_method_mangled_name` with a type-args suffix:
`Map` @ `(int,string)` → `Map__int__string`. Composite args (e.g. `[]int`) get a
structural encoding.

**c. Stamping via a substitution environment (key mechanism).** Install a `codegen`
subst env (`TYPE_PARAM → concrete Type*`) and route type inspection through a new
`codegen_resolve_type()` helper applied at the lowering boundary — `codegen_type_to_llvm`
plus the few sites codegen branches on a type's kind. Then run the *existing*
`codegen_generate_function_decl` under the mangled name.
- **Why bounded:** Tier A's opaque `T` means a generic body never indexes/method-calls a
  `T` value, so every use of `T` flows through LLVM-type lowering; substituting at that
  one boundary covers it. `[]T` is a slice regardless — only its element type resolves.
  This is the payoff of the `any`-only choice.

**d. `type_mapping`.** Add `TYPE_PARAM` handling: under an active subst env, resolve to
the concrete type; a `TYPE_PARAM` reaching lowering *unbound* is an internal error (never
on the non-generic path). Call resolution + predeclare already get-or-create, so instances
slot in unchanged.

## 5. Milestones, testing, risks

**Milestones** (each independently verifiable — the plan's spine):
1. **Front-end** — grammar + AST + tripwire green; `bin/goo --emit-ast` shows
   `type_params` populated on `func Id[T any](x T) T`. No type-checking yet.
2. **Type checker** — typevar scoping, abstract body check, decl invariants, inference +
   `type_substitute`; generic call resolves its result type. Reject-probes pass. Codegen
   still stubbed.
3. **Monomorphization codegen** — worklist + subst env + mangling; end-to-end compile+run.

**Tests:**
- *Positive goldens* (`examples/*.goo` + `.expected.txt`, auto-discovered by
  `scripts/run_golden.sh`): `Id`; `Map`; `Filter`; one generic-calls-generic (transitive
  worklist); the same generic at two distinct type-arg tuples (distinct instances, e.g.
  `Map` at `(int,string)` and `(float64,int)`).
- *Reject-probes* (Makefile heredoc convention, wired into `verify`): un-inferable type
  param; conflicting inference; non-`any` constraint; op-on-opaque-`T` (`x+1`); arity
  mismatch. Each asserts non-zero exit + no `Module verification failed|LLVM ERROR` +
  a specific diagnostic substring.
- *Grammar tripwire*: PASS 81 S/R + 256 R/R before and after.
- Gates: `make verify` + `make test` green (ccomp on PATH from opam `default` switch,
  `~/.opam/default/bin`).

**Ranked risks:**
1. **Grammar delta** (highest) — new `[type_param_list]` on `func_decl`. Mitigation:
   declaration-position placement; goo-grammar skill; tripwire gate.
2. **Codegen substitution-env threading** — the one genuinely new mechanism. Mitigation:
   opaque-`T` confines it to the lowering boundary via a `codegen_resolve_type()`
   chokepoint.
3. **Instance worklist correctness** (symbolic→concrete, transitive). Mitigation: dedup by
   mangled name; standard fixpoint.
4. **`node_type` carrying `TYPE_PARAM`** into codegen. Mitigation: resolve-at-lowering
   handles it uniformly.

**Out of scope (→ future tiers):** explicit call-site instantiation `Map[int](xs)`;
interface-constraint bounds + method/operator use on `T`; generic *type* declarations
(`type Stack[T]`); `comparable`. The dead Task #22 concept/HKT subsystem is left untouched
(neither wired nor deleted here).
