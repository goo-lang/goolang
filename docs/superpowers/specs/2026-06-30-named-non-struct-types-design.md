# Named non-struct types (methods, dispatch, boxing, composite literals)

**Date:** 2026-06-30
**Branch:** `feat/v1-named-type-methods`
**Phase:** v1 Phase 4/5 boundary — surfaced by the TinyGo-style `sort` port.
**North star:** a hand-ported `sort` compiles and runs — `type IntSlice []int`
with `Len/Less/Swap` methods, passed as a `sort.Interface`.

## Problem

`type IntSlice []int` (a named type whose underlying type is non-struct) is a
second-class citizen. Three failures, all rooted in one omission:

1. Composite literal `IntSlice{3,1,2}` → "'IntSlice' is not a struct type"
   (expression_checker.c:226). `var s IntSlice = []int{...}` works, so it is
   purely the named-type composite-literal sugar that is missing.
2. Direct method call `s.Len()` on a named-slice value → "Selector on non-struct,
   non-package type" (expression_checker.c:1231).
3. Boxing a named-slice value into an interface and dispatching → runtime
   SEGFAULT.

## Root cause

`Type` already has a generic `name` field, and `type_receiver_name` falls back to
`type->name` for non-struct types (types.c:733). But `type_check_type_decl`
(type_checker.c ~822/828/849) only stamps the name onto struct/enum/interface
types. So `type IntSlice []int` resolves to a **nameless** `TYPE_SLICE` — the
name (hence the method linkage `IntSlice__Len`) is lost.

## Architecture decision

**Stamp the existing generic `type->name`** (approach A). A named non-struct type
is its underlying-kind `Type` carrying `->name`. Rejected: a `TYPE_NAMED { name,
underlying }` wrapper (approach B) — Go-faithful but forces every
`kind == TYPE_X` site across typecheck + codegen to unwrap first (int→i64-scale
ripple, high risk). A delivers the full scope at a fraction of B's risk; the only
thing it sacrifices — strict named-vs-underlying identity — is not enforced in v1
today.

Scope: **all named non-struct underlying kinds** (slice, map, array, int, func).

## Facets (decompose into ordered, independently-testable units)

### Facet 0 — Naming (keystone)
In `type_check_type_decl`, when `resolved` is not struct/enum/interface, stamp
`resolved->name = strdup(td->name)`. Verify `type_from_ast` returns a fresh Type
(so stamping does not mutate a shared builtin slice/int singleton); if shared,
clone before stamping.
**Acceptance:** `type_receiver_name` of a named slice/int returns the type name;
a method `func (s IntSlice) Len() int` registers as `IntSlice__Len`.

### Facet 1 — Method receivers + selector dispatch
Selector typecheck (expression_checker.c:1231): if the receiver is a named
non-struct type with a registered `Name__method`, resolve the method (signature,
arity) instead of erroring. Codegen: static dispatch to `Name__method` mirroring
the struct method-call path (value/pointer receiver auto-addr/deref as for
structs).
**Acceptance:** `s.Len()`, `s.Less(0,1)` on a named-slice value compile, run, and
return correct values; a method on a named `int` works too.

### Facet 2 — Interface boxing of named non-struct values (segfault)
With Facet 0 in place, vtable/thunk mangling resolves. Systematic-debug the box
+ dispatch crash: a named slice is a multi-field value, so the box (goo_alloc +
store) and the thunk's receiver load must size/load the underlying type
correctly.
**Acceptance:** the recon `sort` shape (named slice boxed as an interface,
dispatched through `Len/Less/Swap`) runs without crashing and sorts correctly.

### Facet 3 — Composite literals for named slice/map/array
Typecheck (expression_checker.c:226) and codegen: when the literal's type-name
resolves to a named `TYPE_SLICE`/`TYPE_MAP`/`TYPE_ARRAY`, route to the existing
slice/map/array composite-literal path using the underlying element/key-value
types. Named `int`/`func` keep rejecting (no composite literal).
**Acceptance:** `IntSlice{3,1,2}`, a named-map literal compile and run.

## Out of scope (follow-up)
Named-type conversions `IntSlice(x)` / `MyInt(5)` — not needed for the `sort`
port (composite literals construct values). Add when a real case demands it.

## Testing
Golden probes per facet (named-slice method call; named-int method; named-slice
interface dispatch — the sort shape; named-slice + named-map composite literals),
each TDD red→green. Capstone: the full single-file `sort` port as a golden probe
once Facets 0–3 land. `make verify` ALL GREEN (incl. ccomp) per commit; golden +
`make test` no regressions. The tuple-index `Swap` idiom stays temp-based (its
grammar fix is a separate deferred milestone).

## Risk
- Facet 0 mutating a shared builtin Type → stamp a clone if `type_from_ast`
  shares. Mitigated by an explicit freshness check + golden regression.
- Facet 2 boxing a >scalar (slice) value — size/load correctness; mitigated by
  the dispatch golden probe asserting real sorted output.
