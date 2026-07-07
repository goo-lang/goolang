# Function Generics — Tier B (Interface-Constraint Bounds) — Design

**Date:** 2026-07-06
**Status:** Approved design, pre-implementation
**Builds on:** Tier A (PR #143 — inference-only, `any`-only, monomorphized generic functions).
**Scope:** interface-constraint bounds on type parameters + method calls on a bounded `T`.
Operators-on-`T` and explicit call-site instantiation are explicitly OUT (later tiers).

## Context

Tier A shipped generic functions with `any`-only, **opaque** type parameters: a `T` value can
only be moved/stored/passed/returned, never inspected. That blocks the whole class of
*constrained* generics (`Print[T Stringer]`, `Max`/`Sort` over a `Less` method, etc.). Tier B
lifts the opacity for the common, well-bounded case: a type parameter bounded by an **interface**,
whose methods may be called on the `T` value.

Verified ground truth (2026-07-06 exploration, branch `feat/function-generics-tier-a`):
- **The bound is already computed and thrown away.** `declare_function_signature`
  (`src/types/type_checker.c` ~810-821) resolves the constraint AST to a `Type* c` purely to
  reject anything that isn't the 0-method `any`, then discards it. `TYPE_PARAM.constraint`
  (`include/types.h` ~178-183) exists but Tier A hard-codes it `NULL` (`type_checker.c` ~753-758).
- **Method-call type-check rejects a `TYPE_PARAM` receiver** at the terminal
  `"Selector on non-struct, non-package type"` (`src/types/expression_checker.c` ~3578); the
  concrete/interface method-set resolution loops sit just above it (~3505-3552).
- **Method-call codegen resolves the receiver by name and does NOT pass through the subst env.**
  `src/codegen/call_codegen.c` ~1174 does `type_receiver_name(recv_type)` → `T__M`; for a
  monomorphized body `recv_type` is the `TYPE_PARAM` (whose top-level `->name` is `"T"`), so it
  mangles the never-emitted `T__M`. The fix pattern already exists ~150 lines below (generic-call
  path substitutes through `codegen->active_subst`).
- **Operators are hard-wired to concrete kinds.** `type_check_arithmetic_op`/`comparison_op`
  (`src/types/expression_helpers.c` ~188-284) gate on `type_is_numeric`/`type_compatible` with no
  type-set/constraint hook — so `<`/`+`/`==` on a bounded `T` needs a *new* type-set constraint
  subsystem. OUT of Tier B.

**Intended outcome:** `func Print[T Stringer](x T)` type-checks, `x.String()` inside it resolves
against the bound, a call `Print(c)` requires `c` to satisfy `Stringer`, and the monomorphized
instance dispatches `x.String()` to the concrete `C`'s method.

## Decisions (locked)

| Axis | Decision |
|---|---|
| Bound kind | An **interface** type (named interface, or `any` = 0-method interface). Non-interface bounds rejected. |
| Body power | Methods in the bound's method set may be called on a `T` value. No operators (deferred). |
| Call-site | Inferred concrete type must **satisfy** the bound (reuse `type_interface_satisfied`). |
| Instantiation | Still **inference-only** (no `Map[int](xs)`); un-inferable type params still rejected at decl. |
| Grammar | **No change** — Tier A's `func_params` reuse already parses `[T Stringer]`. Tripwire stays 82/256. |

## 1. Surface contract

**Compiles:**
```go
type Stringer interface { String() string }

func Print[T Stringer](x T) string { return x.String() }

type Celsius struct { d int }
func (c Celsius) String() string { return itoa(c.d) }

func main() { fmt.Println(Print(Celsius{d: 20})) }   // T=Celsius, satisfies Stringer
```
- `any` remains a valid bound (0-method); method calls on an `any`-bound `T` stay rejected (no methods).
- Multiple bounded params, and a bound satisfied via struct embedding, both work.

**Rejected with clean diagnostics (non-zero exit, no LLVM-verifier crash):**
- Call arg that doesn't satisfy the bound → *"C does not implement Stringer (…)"* (the standard
  satisfaction diagnostic).
- Calling a method NOT in the bound's method set on `T` → *"T has no method Foo"* (or the existing
  no-method diagnostic shape).
- Non-interface bound (`func F[T int](x T)`) → *"type constraint must be an interface"*.
- Operator on a bounded `T` (`a < b`) → the existing numeric/comparable rejection (unchanged).

## 2. Change surface

### Edit 1 — capture the bound (`src/types/type_checker.c`, `declare_function_signature`)
- Where Tier A computes `c = type_from_ast(checker, g->type)` to validate it (~810-821): keep the
  interface check but **relax** it from "0-method `any`" to "any `TYPE_INTERFACE`"; reject
  non-interface bounds with *"type constraint must be an interface"*.
- Thread the resolved bound into the type param: the push site (~753-758) currently builds
  `type_param(name, idx, NULL)`. Restructure so the constraint `Type*` for each param group is
  resolved once and passed as the third arg: `type_param(name, idx, bound)`. (The push loop and the
  invariant loop both walk `func->type_params`; compute the bound per group and reuse it.)

### Edit 2 — method call on `T`, type-check (`src/types/expression_checker.c`, `type_check_selector_expr`)
- Before the terminal reject (~3578), add a `TYPE_PARAM` branch: if
  `expr_type->data.type_param.constraint` is a `TYPE_INTERFACE`, resolve `selector->selector`
  against that interface's method set (reuse the interface-method loop at ~3541-3546) and return the
  matched method's `TYPE_FUNCTION` type. If the method isn't in the bound, emit a no-method error.
- This is also what makes the once-only **abstract body check** accept `x.String()`.

### Edit 3 — method call on `T`, codegen (`src/codegen/call_codegen.c`)
- At the method-call resolution (~1174), resolve the receiver type through the active subst env
  BEFORE mangling: `recv_type = codegen_resolve_type(codegen, recv_type)` (use `type_substitute`
  if a composite/pointer receiver must be handled). Then `type_receiver_name` yields the concrete
  `C` and the lookup finds `C__M`. On the non-generic path `active_subst` is NULL → identity, so
  ordinary method calls are unaffected.

### Edit 4 — call-site satisfaction (`src/types/expression_checker.c`, generic-call inference)
- In the Tier A inference block (`type_check_generic_call`), after inferring `bindings[i] = C` for a
  param whose `TYPE_PARAM.constraint` is a non-`any` interface, check
  `type_interface_satisfied(constraint, C, &method, &reason)`; on failure emit the standard
  *"C does not implement I (reason method m)"* and fail the call. (`any`/0-method bound → always
  satisfied, skip.)

No grammar/AST changes. No new data structures — `TYPE_PARAM.constraint` already exists.

## 3. Milestones, testing, risks

**Milestones** (each independently verifiable):
1. **Bound capture + decl invariant** — `TYPE_PARAM.constraint` populated; non-interface bound
   rejected; `any` still accepted; un-inferable rule intact. (Type-check only; a bounded generic
   still can't call methods yet — that's M2.)
2. **Method call on `T`** — type-check (Edit 2) + codegen (Edit 3) + call-site satisfaction
   (Edit 4). End-to-end: `Print[T Stringer]` compiles, runs, dispatches to the concrete method;
   non-satisfying arg and unknown-method both reject.

**Tests:**
- *Positive goldens* (`examples/genb_*.goo` + `.expected.txt`): `Print[T Stringer]` → concrete
  method dispatch; a bound method whose result feeds a computation; a bound satisfied via **struct
  embedding** (concrete promotes the method); two different concrete types through one bounded
  generic (distinct instances each dispatching correctly).
- *Reject-probes* (Makefile `generics-bound-reject-probe`, wired into `verify`): non-satisfying arg;
  method not in bound; non-interface bound; operator on bounded `T` still rejected. Each asserts
  non-zero exit + no `Module verification failed|LLVM ERROR` + a specific diagnostic.
- Gates: `make verify` ALL GREEN; golden count grows by the new positives; `make test` 76/1;
  grammar tripwire **82/256 unchanged** (no parser change).

**Ranked risks:**
1. **Codegen receiver resolution (Edit 3)** — the one load-bearing change. Mitigation: mirrors the
   existing generic-call substitution; `active_subst` NULL on the concrete path keeps ordinary
   method calls untouched (golden regression is the guard).
2. **Promoted/embedded methods on the concrete type at dispatch** — a bound satisfied via embedding
   must still dispatch through the existing promoted-method codegen after substitution. Mitigation:
   an explicit embedding golden; if the direct `C__M` lookup misses a promoted method, fall back to
   the embedding-resolve path the interface/method codegen already uses.
3. **`type_receiver_name(TYPE_PARAM)` returning `"T"`** — the reason Edit 3 is needed; ensure the
   resolve happens on every method-call receiver path, not just the common one.

**Out of scope (→ later tiers):** operators on `T` (needs constraint type-sets — a new subsystem);
`comparable`/`Ordered`/`Numeric` built-in constraints; explicit call-site instantiation
`Map[int](xs)`; generic type declarations; multiple-interface / union bounds.
