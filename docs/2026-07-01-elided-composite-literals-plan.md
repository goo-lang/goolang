# Elided composite literals (plan)

Date: 2026-07-01

## Goal

Support Go's elided-type composite literals within typed array/slice literals:

```go
ps := []pair{ {1, 2}, {3, 4} }          // positional
qs := []pair{ {lo: 1, hi: 2}, {hi: 9} } // keyed
var t = [2]pair{ {1, 2}, {3, 4} }       // array, global
```

Go spec: "Within a composite literal of array, slice, or map type T, elements or
map keys that are themselves composite literals may elide the respective literal
type if it is identical to the element or key type of T."

This is the most pervasive Go syntax still missing. It unblocks `unicode/utf8`
`DecodeRune` (the `acceptRanges` struct table), `unicode` `RangeTable`s, and
essentially every struct-slice table in real stdlib source.

## Scope (this PR)

- Array + slice element context only (NOT map values — separate follow-up).
- Positional AND keyed elided struct literals (reuse existing struct_lit_inits).
- Element type T must be a struct (the common case). Non-struct elided is a
  type error with a clear message.

## Design

### Parser
Add `composite_elem` = `expression | LBRACE struct_lit_inits RBRACE | LBRACE RBRACE`
and `composite_elem_list`. The brace form builds a `StructLiteralNode` with
`type_name == NULL` (the "elided" marker), reusing struct_lit's field-name
piggyback extraction. Wire the four typed-literal rules (slice/array ×
with/without trailing comma) to use `composite_elem_list` instead of
`expression_list`.

Conflict risk: LOW. `LBRACE` is not a valid `expression` start, so there is no
first-set overlap between the two `composite_elem` alternatives. Judge by
behavior (full suite + probes), not raw conflict count (established lesson).

### Type-checker
Thread the expected element type into elided literals: in `check_slice_elements`
and the array-literal element loop, before `type_check_expression(e)`, if `e` is
an elided struct literal (`AST_STRUCT_LITERAL`, `type_name == NULL`), stamp
`e->node_type = want_elem`. In `type_check_struct_literal`, when
`type_name == NULL`, use the pre-stamped `expr->node_type` as the target struct
type instead of the by-name variable lookup.

### Codegen
No change. `codegen_generate_struct_lit` already resolves the struct type from
`expr->node_type` (line 590); `type_name` is used only for error strings and
enum-variant lookup (not reached for a TYPE_STRUCT). Verified.

## Alternatives considered

1. **Add an `expected_type` parameter to `type_check_struct_literal`.**
   Pro: explicit. Con: it is called from the generic expression dispatch
   (`type_check_expression`), so every caller and the dispatch signature would
   change — invasive for no behavioral gain. Rejected.

2. **A dedicated `AST_ELIDED_COMPOSITE` node kind.**
   Pro: type-safe marker. Con: a new node type ripples through free/copy/print
   and codegen dispatch; `StructLiteralNode` with `type_name == NULL` already
   carries exactly the needed data and flows through existing codegen. Rejected
   in favor of the NULL-type_name sentinel (localized, no new plumbing).

3. **Node_type-slot pre-stamp (chosen).** Reuses the existing in/out `node_type`
   convention (the same slot struct_lit already round-trips). Minimal surface,
   no signature changes, codegen unchanged.

## Test (TDD)

Golden probe `elided_composite_literal`: positional + keyed elided elements in
both slice and array literals, local and global scope, field reads and range.

## Gate

`make verify` (golden), `make test`, `make ccomp-link` — all must stay green /
byte-identical.
