# Per-type × per-value composition (design)

Sub-project 2 of the SPMD/parallel-lanes keystone (Leg 3 of the hybrid-memory direction).
Sub-project 1 (merged, PR #160) delivered single-axis comptime-value specialization:
`func fill(comptime n int, seed int)` monomorphized per value. This sub-project composes
that axis with the existing generic type-parameter axis, delivering the full SPMD
"per-type kernel × per-tile" shape:

```goo
func kernel[T any](comptime n int, data T) T {
    var buf [n]T          // comptime length × generic element — the forcing function
    ...
}
kernel(4, 10)             // T inferred int64  -> instance kernel__int64__n4
kernel(2, 3.5)            // T inferred float64 -> instance kernel__float64__n2
```

Grounding: the two-axis machinery map (2026-07-08, main @ a9cf5a6). All anchors below
cite it. Sub-project 1's design (2026-07-07 doc) remains normative for the single
comptime axis; this doc only specifies the composition.

## Surface & semantics

- A function may declare BOTH `[T ...]` type parameters and `comptime` value parameters.
  This lifts the primary wall at `declare_function_signature`
  (src/types/type_checker.c:807-828, "not yet supported together with type parameters").
- Type arguments are inferred per the existing generic machinery — inference-only
  (explicit call syntax `kernel[int](4, x)` does not parse today: the `[int]` is
  taken as an index expression, a pre-existing limitation of ALL generic calls,
  not specific to composition); comptime arguments remain explicit
  compile-time-constant ints (literals, const exprs, `const`, `comptime const`)
  exactly as in sub-project 1.
- A comptime parameter's own declared type must be a plain concrete type (`int`):
  `comptime n T` (typed by a type parameter) is REJECTED at declaration — a value whose
  comptime evaluation depends on the type axis is out of scope.
- Comptime parameter positions never participate in type-argument unification: at a call
  to `kernel[T](comptime n int, data T)`, argument 1 type-checks as `int` and is captured
  as a comptime value; only non-comptime positions feed `unify_types`.
- Instance identity is the tuple (function, type-args…, comptime-values…). Same tuple at
  two call sites → one instance; any component differing → distinct instances.
- Inside the body, `n` is a compile-time constant (drives `[n]T`, `[n]int`, loop bounds)
  and `T` is the bound type — both per instance, never the template placeholder.
- A composed function is NOT a first-class value: `Type.data.function.has_comptime_params`
  is set as today, so every value-escape wall from sub-project 1 (assignment, args,
  composites, sends, returns, interface satisfaction) applies unchanged. Generic
  functions already cannot be used as values pre-instantiation; composition inherits the
  stricter of the two.
- `go kernel(4, data)` and `defer kernel(4, data)` dispatch to the correct combined
  instance (goroutine-per-lane is the point of the leg).

## Mechanism (decisions, with the map's anchors)

1. **Seeding/keying: extend `GenericInstantiation`, no third struct.** Add tail fields
   `int64_t* comptime_values; size_t comptime_value_n;` (include/types.h:455-460,
   struct-tail convention). A generic-only seed has `comptime_value_n == 0`; a composed
   seed carries both payloads (each independently owned, `fn` non-owned, matching
   type_checker.c:113-136 discipline). Comptime-only functions keep their existing
   separate list — no churn on the proven single axis.

2. **Checker capture: fold comptime capture into `type_check_generic_call`.** The
   dispatch at expression_checker.c:3218 early-returns before the comptime capture loop
   (3346-3465), so today a generic callee never captures. Extract the per-argument
   comptime validation/capture (two-tier fold: `goo_fold_const_int_ctx` then
   `comptime_eval_expression`; `func_decl_param_at`; append to
   `call->comptime_value_args`) into a shared helper used by BOTH
   `type_check_call_expr`'s loop and `type_check_generic_call`'s loop (3rd near-copy
   avoided per the map's walker warning). In the generic loop, a comptime position:
   validates as its declared concrete type, captures the value, and is EXCLUDED from
   unification. Record via `type_check_record_instantiation` extended to carry the values.
   `type_check_generic_call` must honor the same first-visit contract
   (`expr->node_type == NULL` discriminator, expression_checker.c:2662) — codegen
   re-invokes `type_check_call_expr` on the same node (call_codegen.c:113/1415/1798) and
   the dispatch forwards to the generic path.

3. **Mangling: types first, then values — `base__<typetok>...__n<value>...`.**
   Reuses both existing schemes as segments (codegen_mangle_instance monomorphize.c:61-68,
   codegen_mangle_comptime_instance 160-174). Collision safety: within one function the
   type-arity and value-arity are fixed by the declaration, so segment counts are
   unambiguous; across functions base names differ (one symbol namespace,
   monomorphize.c:782-787). The residual theoretical collision (user type literally named
   `n4` as a type arg vs a value segment) is arity-blocked for any single function; a
   probe pins this if the shape is constructible.

4. **Instance generation: one combined generator, both envs installed.** Extend
   `codegen_generate_function_instance` (monomorphize.c:76-153) to also install
   `active_comptime_values/active_comptime_value_n` (CodeGenerator already carries both
   field sets independently — codegen.h:183-184, 245-246; today no path sets both). It
   already pushes checker-side `active_type_params` and codegen-side `active_subst`;
   the comptime rebinding then happens in `codegen_generate_function_decl`'s existing
   mirror-scope loop (function_codegen.c:1006-1062) untouched. Save/restore BOTH axes'
   fields unconditionally (the map flags that each generator currently leaves the other
   axis's fields alone — a composed generator must not leak either).

5. **Template body-check: both bindings coexist.** `T` pushed abstract
   (type_checker.c:1038-1052) and `n` bound to the placeholder (1105-1127) write disjoint
   field sets — structurally safe per the map. `[n]T` in the template produces a
   `comptime_length`-flagged array of TYPE_PARAM element; the per-instance re-derivation
   (function_codegen.c:1543-1566, composite_codegen.c:1329-1341) runs `type_from_ast`
   under the instance's rebound `n` AND the pushed type params, yielding the real length
   with the element still TYPE_PARAM — LLVM lowering substitutes via `active_subst`
   (type_mapping.c:28-37). This is the load-bearing composition point; Task-level tests
   force it with `[n]T`.

6. **Call-site rewriting: three-way dispatch, not if/else-if.** call_codegen.c:1451
   (generic) + 1494 (`!func_val` comptime) becomes: both present → combined mangler;
   else generic-only; else comptime-only. The `!func_val` guard pattern is the map's
   sharpest bug-in-waiting — a composed call today would silently dispatch on the generic
   axis alone. Same three-way logic in `codegen_generate_go_stmt`
   (statement_codegen.c:1719-1744, currently comptime-only): the generic half of the
   go-stmt rewiring does not exist yet and is REQUIRED SUBSTRATE built here (fixes the
   pre-existing `go GenericFn(...)` gap as a side effect — pin with its own test).

7. **Nested/transitive discovery: extend the existing recursion, same shared state.**
   `collect_generic_calls` (monomorphize.c:303-485) already collects both axes' fields.
   `mono_instantiate` (552-644) gains the combined case: a nested call with both
   type args (substituted under the enclosing env, concrete-checked via
   `args_contain_typeparam`) and literal comptime values (transitive forwarding stays
   rejected) recurses into the combined generator. Thread the SAME `seen`/`stamped_count`
   pair (762-763) — parallel bookkeeping breaks dedup/cycle-guard, per the map.

## Walls: lifted / kept

- LIFTED: `declare_function_signature` comptime+type-params rejection (type_checker.c:807-828).
- KEPT (unchanged from sub-project 1): methods (830-856); imported-package functions
  (858-888); transitive comptime forwarding; not-first-class-value walls.
- KEPT: comptime-length array VALUES binding a generic type parameter
  (expression_checker.c:2489-2501, wall b). Lifting it means keying instances on
  concrete array types including instance lengths — a different, larger feature than
  declaring both axes on one function. The existing diagnostic stands.
- NEW WALL: `comptime n T` — comptime parameter typed by a type parameter, rejected at
  declaration ("comptime parameter type cannot be a type parameter (not yet supported)").

## Scope (YAGNI, first cut)

- Comptime `int` values only; explicit comptime arguments; type args inferred only.
- Explicit type-argument call syntax: not supported (open point 1 resolved — rejected
  as index expression; diagnostic quality is a pre-existing generics-wide follow-up).
- One function, both axes — values crossing BETWEEN separately-declared functions'
  axes stay governed by wall (b).
- No comptime-dependent type-level computation (`[n]T` yes; `T[n]`-style type-level
  arithmetic or conditional types, no).
- Methods and package functions with either or both axes: still rejected (follow-ups).

## Testing

- Golden `comptime_generic_compose_probe.goo`: `kernel[T any](comptime n int, seed T) T`
  with a `[n]T` buffer; instances (int64,4), (int64,2), (float64,4); prints per-instance
  sums; IR check greps three distinct symbols `kernel__int64__n4`, `kernel__int64__n2`,
  `kernel__float64__n4` and distinct alloca sizes; a repeated (int64,4) call site proving
  dedup (3 distinct instances from 4 call sites).
- `go kernel(4, data)` result via channel (golden); `defer` variant.
- Nested: a composed function calling another composed function with literal args.
- Reject matrix additions: `comptime n T` declaration; wall (b) unchanged (existing case
  stays green); a composed function used as a value (assignment) rejects; runtime value
  to the comptime position of a generic call rejects; generic-only and comptime-only
  regression cases stay green (existing 16 walls untouched).
- Gates throughout: golden suite (currently 313/0) + matrix (16/16 growing) + unit
  (76/1-skip) + grammar tripwire 82 S/R + 256 R/R exact (no parser.y changes are
  expected in this sub-project; the tripwire is a pure guard).

## Open verification points for the implementer

1. Whether explicit type-argument syntax (`kernel[int](4, x)`) parses today for calls —
   the composition must work with inference regardless; explicit syntax is
   verify-and-pin, not build.
2. `type_check_record_instantiation`'s callers and frees (type_checker.c:475-487,
   113-136) — extending the struct tail must update the free path for the new payload.
3. Whether `codegen_resolve_callee` or the predeclare skip-guards
   (function_codegen.c:679-695, codegen.c:439-456) need a combined-case adjustment —
   both today return early on either axis independently, which composes correctly for
   skipping, but verify no path predeclares a bare template symbol for a composed
   function.
4. The interaction of `checker->active_type_params` save/restore with the comptime
   placeholder binding when BOTH run in `type_check_function_decl` — confirm the
   restore order is LIFO-consistent.

## Related follow-ups (recorded from PR #160's review loop, unchanged by this doc)

Method/package/cross-package specialization; transitive comptime forwarding; comptime
types beyond int and call-site inference; array slicing (v1 language limit);
`type_from_ast` sibling error-path list leaks; failed-`:=` cascade residue; multi-assign
diagnostic column anchoring. Wall (b) lifting joins this list (superseded scope note
above). This sub-project addresses only the "per-type × per-value axis composition"
entry.
