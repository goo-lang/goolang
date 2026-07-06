# Interface-Target RTTI — `x.(Stringer)`, `case Stringer:`

Date: 2026-07-06
Status: Design approved (Approach 1 — closed-world enumeration)
Related: PR #114 (concrete type assertions/switches), PR #135 (concrete RTTI on `any`),
PR #137 (per-type descriptor behind vtable slot 0), `goolang-rtti-followups` memory.

## Problem

Type assertions and type switches to an **interface target** are rejected:

```goo
var x interface{} = someStringer
s := x.(Stringer)              // "type assertion to an interface type is not supported in v1"
switch v := x.(type) {
case Stringer:                 // same rejection
    ...
}
```

Only **concrete** targets work today (`x.(int)`, `x.(*Point)`, `case Point:`), via
vtable-pointer identity. Asserting to an interface needs to answer, at runtime,
"does `x`'s dynamic type implement `Stringer`?" and, if so, produce a `Stringer`
value with the right method vtable. This is the last deferred piece of the RTTI
arc; PR #137's per-concrete-type descriptor is its foundation.

## Key facts this builds on

- An interface value is the 2-word box `{ ptr vtable, ptr data }`.
- After #137, the vtable's **slot 0 is a pointer to the concrete type's
  descriptor** (`goo.typedesc.<T>`), and that descriptor pointer is the
  **dynamic-type identity** — the same descriptor is shared across every
  interface a given concrete type is boxed into.
- `type_interface_satisfied(checker, I, T)` answers "does concrete `T` implement
  interface `I`?" **at compile time**.
- `codegen_interface_vtable(codegen, checker, I, T, pointer_form)` emits (or
  reuses) the per-`(T, I)` vtable — slot 0 = `T`'s descriptor, slots 1..n = `T`'s
  thunks for `I`'s methods.
- Declared types live in the checker's scope as a `Variable*` linked list
  (`Scope.variables`), iterable (precedent: `type_checker.c:146`).

## Chosen approach — closed-world enumeration (Approach 1)

Goo compiles the whole program, so the set of concrete types is known at compile
time. At each interface-target site, enumerate the concrete types that implement
the target interface `I`, and emit a runtime chain that compares `x`'s dynamic
descriptor (`box.vtable[0]`) against each implementer's descriptor. On the first
match with type `T`, construct the `I` value `{ (T, I) vtable, box.data }`. No
match → comma-ok `false` / panic.

This reuses the descriptor identity (#137), the per-`(T, I)` vtable generator,
and the compile-time satisfaction check — **no new runtime metadata**.

### Rejected alternatives

- **Per-type itab table in the descriptor** (each descriptor lists
  `(interface_id, vtable)` it implements; `x.(I)` looks up `I`'s id at runtime):
  scales better and is closer to Go's itab, but needs a stable interface-identity
  scheme, per-type itab construction (which still enumerates interfaces), and
  larger binaries. More than a closed-world v1 needs. Keep as the escape hatch if
  enumeration's code growth ever bites.
- **Runtime method-set reflection** (descriptors carry method names + fn-ptrs;
  assemble the target vtable at runtime): most general (open-world) but heaviest;
  unnecessary when the world is closed.

## Components

### 1. Typecheck — allow interface targets, enumerate implementers

- **Assertion** (`expression_checker.c` ~552): remove the "interface target not
  supported" rejection. For an interface target on an interface/`any` operand,
  the assertion is *always well-formed* (Go: runtime-checked). Do **not** require
  static satisfaction between the operand interface and the target interface.
  Stamp `expr->node_type = target_interface`.
- **Type switch** (`type_checker.c` ~2321): the concrete-case path calls
  `type_interface_satisfied(iface_type, case_type)` and rejects on failure. For a
  case whose type is an **interface**, skip that concrete-satisfaction rejection
  and accept it (runtime-checked); the bound `v` in that case has the case's
  interface type.
- **Enumeration**: at each interface-target site, walk to the root scope and
  collect every declared concrete type `T` (`TYPE_STRUCT`, and pointer forms with
  a nameable receiver) for which `type_interface_satisfied(checker, I, T)` holds.
  Stash the resulting `Type*` list on the AST node (a tail-appended field on
  `TypeAssertNode` / `TypeCaseNode`) so codegen consumes it without re-deriving.
  An empty list is legal (the assertion simply always fails at runtime).

### 2. Codegen — the shared match/build primitive

`codegen_interface_target_match(codegen, checker, iface_val, src_iface,
target_iface, implementers, n) → { LLVMValueRef match_i1, LLVMValueRef built }`:

1. Extract `vtab = box.vtable` (field 0) and `data = box.data` (field 1).
2. Guard: `vtab == null` (nil interface) → `match = false`, `built = zero I`.
3. Else `desc_have = vtab[0]` (runtime dynamic-type descriptor).
4. For each implementer `T`: `desc_want = codegen_get_or_emit_type_desc(T, form)`;
   `eq = icmp desc_have, desc_want`. On the matching branch, build
   `iv = { codegen_interface_vtable(target_iface, T, form), data }`.
5. Combine the per-`T` results into `match_i1` (OR of the eqs) and `built` (a phi
   selecting the `iv` of whichever `T` matched, else zero `I`). Implementers are
   tried in a deterministic order.

This one primitive serves all three lowerings. It mirrors the structure of the
existing concrete `codegen_interface_assert_match` but compares the descriptor
(dynamic type) rather than the whole `(T, src)` vtable, and *constructs* a new
interface value rather than unboxing a concrete.

### 3. Lowerings

- **`x.(I)` single-return**: run the primitive; if `!match`, panic via #137's
  `goo_panic_iface_conversion` (dynamic name from the descriptor:
  `interface conversion: <dyn> is not I`). Result is `built`.
- **`v, ok := x.(I)`**: result is `(built, match_i1)` — `built` is the zero `I`
  when `ok` is false, never dereferenced by correct comma-ok code.
- **`switch v := x.(type) { case I: }`**: in the type-switch codegen, an
  interface case emits the primitive's match chain; on match it binds `v` to
  `built` (the target-interface value) and branches to the case body. Interface
  cases interleave with concrete/`nil` cases **in source order** — first match
  wins (Go-faithful), which the existing sequential case-test structure already
  provides.

## Data flow

```
x.(I)   x: interface   I: interface target
  box.vtable == null        -> no match (nil interface)
  desc = box.vtable[0]       (dynamic-type identity)
  for T in implementers(I):
      if desc == typedesc(T):  built = { vtable(T,I), box.data };  match = true
  single-return: !match -> panic "interface conversion: <desc.type_name> is not I"
  comma-ok:      (built, match)
  switch case I: match -> bind v = built, run body
```

## Error handling

- **Nil interface** (`{null,null}`): never matches — comma-ok `false`, `x.(I)`
  panics, `case I:` falls through. The null-vtable guard prevents dereferencing
  `vtable[0]`.
- **No implementer matches**: comma-ok `false`, or panic with the dynamic type
  name (`<nil>` if the value was a nil interface).
- **Zero implementers of I in the program**: legal; the assertion always fails at
  runtime. (A static "impossible" warning is out of scope.)

## Testing

Golden + abort probes (each go-run-verified where behavior is defined):

- `iface_target_assert` — `x.(I)` succeeds for an implementer; the returned value
  dispatches `I`'s methods correctly.
- `iface_target_commaok` — `v, ok := x.(I)` true for an implementer, false for a
  non-implementer and for a nil interface.
- `iface_target_switch` — `switch x.(type)` mixing `case I:`, a concrete case, and
  `case nil:`, asserting source-order precedence (a value matching both a concrete
  and an interface case takes the first).
- `iface_target_multi` — a type implementing two interfaces asserts to each.
- `iface_target_ptr` — a pointer-receiver implementer (`*T` satisfies `I`).
- `iface-target-assert-abort-probe` — a failed `x.(I)` panics, message names the
  dynamic type (`is not I`).

Regression net (shared vtable/descriptor machinery): the full
interface-method-dispatch, embedding, map-key, and concrete
assertion/type-switch suites; `make verify` ALL GREEN; golden 0 failures;
`make test` 76/1; bison baseline unchanged (no grammar change expected — the
grammar already parses interface names in target/case position).

## Open questions (resolve during planning)

1. **Enumeration timing** — compute implementers at type-check and stash on the
   node (chosen default, cleaner) vs re-derive at codegen by walking the root
   scope. Confirm the node-field approach and where the field lives.
2. **`built` phi vs per-branch construction** — assemble the target value via a
   phi over match branches, or a dedicated basic-block chain. Pick the shape that
   keeps the type-switch and assertion lowerings sharing one code path.
3. **Panic wording** — `interface conversion: <dyn> is not I` (mirrors the
   concrete message) vs Go's `... does not implement I (missing method M)`. v1:
   the simpler `is not I`, consistent with the concrete-assertion panic.
4. **`x.(any)`** — asserting to the empty interface trivially matches every value;
   confirm it's allowed and produces an `any` value (likely a no-op rebox).

## Risks

- **Code growth**: N descriptor comparisons per interface-assertion site
  (N = implementers). Acceptable for closed-world v1; log if a site emits an
  unusually large chain. Approach 2 (itab table) is the escape hatch.
- **Shared-machinery regressions**: the primitive reuses the vtable/descriptor
  path that map-keys, dispatch, and concrete assertions depend on. The
  whole-branch final review must re-run the full interface suite (per the
  "final review is load-bearing" lesson).
- **Enumeration completeness**: missing an implementer would make a valid
  assertion wrongly fail. The scope walk must reach every declared type; verified
  by a probe asserting a type declared in a different position than the boxing
  site.
