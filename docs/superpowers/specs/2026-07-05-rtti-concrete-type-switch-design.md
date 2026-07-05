# Design: concrete-type RTTI on the empty interface

**Date:** 2026-07-05
**Branch:** `feat/rtti-concrete-type-switch` (base main @ 1111f65)
**Queue item:** type switches `switch v := x.(type) { case int: … }` and type
assertions `x.(int)` on `any` / the empty interface, currently rejected with
"not supported in v1 (requires runtime type information)".

## Problem

Type switches and type assertions already work on **non-empty** interfaces via
a vtable-pointer identity compare (`iface.vtable == &goo.vtable.<T>.<I>`). They
were rejected on the **empty** interface (`interface{}` / `any`, method_count
== 0) because *before #132* a zero-method vtable was an identical `[0 x ptr]`
array for every concrete type, so the compare always matched — a silent
miscompile, guarded against rather than allowed.

The interface-map-keys work (#132) prepended a per-concrete-type value-equality
function at **vtable slot 0** (methods shifted to 1..n). An `any` vtable is now
`[1 x ptr]` — a distinct global (`goo.vtable.<T>.any`, deduped by name, stable
address) per concrete type. The guard's premise is therefore **stale**:
vtable-pointer identity now distinguishes concrete types under `any` too.

A throwaway spike (lift the two empty-interface guards, test, revert) confirmed
that with **no other change**, concrete discrimination on `any` matches `go run`
exactly: type switch over `int`/`string`/`*T`/`nil`/`default`, `x.(int)`, and
comma-ok hit/miss all produced identical output.

## Scope

**In:** concrete-type discrimination on the empty interface — type switch with
concrete cases (`int`, `string`, `bool`, `*T`, named structs, `nil`), the
`x.(ConcreteType)` assertion, and its comma-ok form, on an `any` operand.

**Out (deferred to a later cycle):** assertion/switch to an **interface** target
(`x.(Stringer)`, `case Stringer:`) — needs a per-concrete-type descriptor
enumerating implemented interfaces; stays rejected with the existing message.
Also out: dynamic-type-name panic messages (needs a name descriptor),
exhaustiveness checking, hashing.

## Approach

Remove the two stale empty-interface guards; rely on the existing, spike-proven
vtable-identity mechanism. No new runtime machinery, no codegen change, no
grammar change.

**Chosen over** adding a per-concrete-type type descriptor now (the invasive
3-word-repr / side-table option the #132 spec explicitly declined): the
descriptor is only needed for interface targets and dynamic-type names, both
out of scope here. Lifting the guards is the smallest change that ships the
pervasive concrete-discrimination case, and the descriptor remains a clean
separate cycle.

## Components

1. **`src/types/expression_checker.c`** — remove the `method_count == 0` reject
   in the `AST_TYPE_ASSERT` case (~line 554). Keep the interface-target reject
   just below (~569). This covers `x.(T)`, its comma-ok form, and the
   `v := x.(T)` decl form (all route through this case).
2. **`src/types/type_checker.c`** — remove the `method_count == 0` reject in
   `type_check_type_switch_stmt` (~line 2274). Keep the interface-target reject
   in the case loop (~2328).
3. **Codegen** — unchanged. `codegen_interface_assert_match`
   (interface_codegen.c) and `codegen_generate_type_switch_stmt`
   (statement_codegen.c) already build the compare against a get-or-emit'd
   `goo.vtable.<CaseType>.<Iface>`, which for an `any` operand is
   `goo.vtable.<CaseType>.any`.

## Data flow

`switch v := x.(type) { case int: … }` with `x : any` →
codegen loads the `{vtable,data}` box → for each case emits/references
`goo.vtable.int.any` (get-or-emit, deduped) → compares the box's vtable word →
on match, binds `v` at the case's concrete type and runs the arm. The incoming
box was created at its boxing site with the same-named vtable (boxing has no
`method_count == 0` shortcut — verified), so the addresses match.

## Error handling

- **Assert miss** (`x.(int)` when `x` holds a string): `goo_panic`, non-zero
  exit. Message uses **static** type names (`interface conversion: interface is
  not int`) — Go names the dynamic type, which needs a name descriptor (out of
  scope). Documented deviation; keep static.
- **Nil `any`**: `case nil` matches; `x.(int)` on nil panics; comma-ok yields
  `false`. (Spike-confirmed for switch; goldens cover assert/comma-ok nil.)
- **Interface target** (`x.(Stringer)`, `case Stringer:`): stays rejected with
  the existing "type assertion to an interface type is not supported in v1
  (concrete target types only)" message. Explicitly out of scope.

## Testing

The guards existed for a real pre-#132 reason, so this is verified
exhaustively (the "final review is load-bearing" lesson — probe degenerate
shapes, not just the happy rvalue path).

**Safety audit (before/after):** enumerate every empty-interface
discrimination path and confirm each reaches the per-type vtable, not a
shared/null one:
- expression assert `x.(T)`, comma-ok `v, ok := x.(T)`, decl form `v := x.(T)`;
- type switch with bind (`v := x.(type)`) and without (`x.(type)`);
- named `type Empty interface{}` vs literal `interface{}` vs predeclared `any`
  as the operand type — all must box through `goo.vtable.<T>.<thatIface>`.

**go-run goldens** (expected output from `go run`, never hand-written):
- type switch over `any` with `int`, `string`, `bool`, `*T`, a named struct,
  `nil`, and `default`;
- multi-type case `case int, string:` — `v` retains the `any` type in that arm;
- comma-ok hit and miss (`s, ok := x.(string)`);
- plain assert hit (`x.(int)`);
- `any` passed through a function parameter and returned from a function.

**Reject / panic probes** (Makefile, wired into `verify:`):
- assert-miss (`x.(int)` on a string-holding `any`) runtime-panics with
  non-zero exit;
- `x.(Stringer)` still rejected at compile time (interface target);
- `case Stringer:` in a type switch still rejected.

**Known limitation** (documented, not fixed): Goo conflates `int` ≡ `int64`, so
`case int:` and `case int64:` collide — pre-existing, out of scope.

**Gates:** bison tripwire 82 S/R + 256 R/R unchanged (no grammar change); golden
suite count grows, 0 failed; `make verify` (incl. CompCert `ccomp-build` /
`ccomp-link`) exit 0; CI `tests` + `demos` green.
