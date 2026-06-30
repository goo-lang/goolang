# Interfaces nested in slices and structs

**Date:** 2026-06-30
**Branch:** `feat/v1-phase4-iface-containers`
**Phase:** v1 Phase 4 (interfaces), follow-up to PR #49 (`=` assignment boxing)
**North star:** TinyGo-style `sort` — `sort.Stable` / reverse-wrappers store an
`Interface` in a struct field; a `[]Shape` is the common polymorphic-collection
shape.

## Problem

A concrete implementer used as a **slice element** or **struct field** of
interface type is rejected at typecheck (then cascades):

- `[]Shape{Sq{...}}` → "Slice literal element 0 type '(null)' is not compatible
  with declared element type '(null)'".
- `Holder{sh: Sq{...}}` (field `sh Shape`) → "Cannot use (null) as field 'sh' of
  type (null)".

var-decl init, return, call-arg, and plain `=` boxing already work (PRs #45–#49);
composite-literal element/field positions are the remaining gap.

## Root cause + precedent

The composite-literal typecheck uses raw `type_compatible`; codegen stores the
concrete value directly. The nullable `?T`-in-container case is the exact same
shape and is already solved (P2-4 struct field, P2-5 slice element): typecheck
permits the bare value and codegen auto-wraps it into the `{i1,T}` nullable
struct. Interface boxing is the analogue — wrap into the `{vtable,data}` struct.

## Design

Reuse the assignment-gap machinery (`check_interface_assign`,
`codegen_interface_box`) at the container sites:

### Typecheck — `src/types/expression_checker.c`

1. Typed slice `[]Iface{...}` element check (~line 120): when the declared
   element type `want` is `TYPE_INTERFACE`, route through
   `check_interface_assign(checker, et, want, e->pos)` instead of
   `type_compatible`.
2. Struct literal named-field check (~line 259) and positional-field check
   (~line 277): when the field type is `TYPE_INTERFACE`, route through
   `check_interface_assign`.

### Codegen — `src/codegen/composite_codegen.c`

3. Struct field (after the P2-4 nullable-wrap block, ~line 481): if `field_type`
   is `TYPE_INTERFACE` and `val->goo_type` is concrete (`!= TYPE_INTERFACE`), box
   via `codegen_interface_box(codegen, checker, field_type, val->goo_type,
   val->llvm_value)` and set `val->goo_type = field_type`. The value is already
   loaded (lvalue handled above); the integer-width fixup below is a no-op on the
   resulting aggregate.
4. Slice element (after the P2-5 nullable-wrap block, ~line 905): load the
   element if it is an lvalue, then if `elem_type` is `TYPE_INTERFACE` and the
   element is concrete, box via `codegen_interface_box`.

interface→interface in a container needs no box (same layout) — only concrete→
interface boxes.

## Testing (TDD, golden-probe driven)

New `examples/interface_container_probe.goo`:
- `[]Shape{Sq{3}, Rect{2,3}}` then dispatch `.Area()` per element;
- a struct with an interface field `Holder{sh: Sq{5}}` then `h.sh.Area()`.
- assert printed output via `.expected.txt`.

Write it FAILING first, then implement typecheck → codegen. Gate: golden stays
green (currently 78/0), `make test` 76/1, interface probes pass, ccomp `make
verify` ALL GREEN.

## Alternatives considered

- **Separate interface-in-container path** — rejected: duplicates the nullable
  precedent and the assignment-gap consolidation. Reusing `check_interface_assign`
  + `codegen_interface_box` keeps one definition of "satisfies" and "box."

## Risk

The boxed element/field LLVM type must match the container's declared element/
field type (`{vtable,data}`). Mitigated by the golden probe asserting real
dispatched output, not just a clean compile.
