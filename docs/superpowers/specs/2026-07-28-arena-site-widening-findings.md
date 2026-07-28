# Widening the arena's allocation sites — findings before implementing

**Date:** 2026-07-28
**Status:** design findings. No compiler code changed.
**Follows:** ADR 0002 (accepted), phase 1.

**Why a findings pass first.** The soundness rule here is asymmetric and
unforgiving: `block_escape.h` says `true` (escapes) is always safe, while
`false` asserts "provably does not outlive the block". **Under-marking is the
only bug class that can dangle a pointer** — a use-after-free, not a lost
optimisation. A wrong `false` produces a program that runs, passes its golden
diff, and corrupts memory under a different allocator or optimisation level.
That is worth a reading pass before a coding pass.

## What phase 1 actually requires

Both halves, or neither has any effect:

1. `block_escape` classifies only `new(T)` and `&<composite literal>`. Its own
   header says every other allocation "is out of scope for this cut … a lookup
   finds no decision for them -> miss -> heap".
2. 11 of 13 `codegen_emit_alloc` call sites pass `NULL` as the alloc site, and
   `block_escape_site_escapes(NULL)` returns `true` by contract.

Threading the node through without extending the analysis yields a miss.
Extending the analysis without threading the node yields nothing to look up.

## The site kinds are NOT equally tractable

This is the finding that changes the plan. They were treated as one list; they
are two very different problems.

### Tractable now — closure and method-value environments

`function_codegen.c:955` already has `lit`, the `FuncLitNode`, in scope at the
allocation. `composite_codegen.c:659` has the selector expression for a
method-value receiver cell. Both are ordinary AST nodes.

The semantics line up with the existing machinery exactly: **an environment
escapes the block iff the closure value escapes it.** Register the `FuncLit`
node as a site, and the existing taint propagation — which already tracks a
value through assignment, call arguments, returns and `go` statements — gives
the right answer with no new concept.

One known interaction to handle explicitly rather than discover: `discover_expr`
deliberately does NOT recurse into a `FuncLit` body (it mirrors
`param_escape.c`'s choice). That is a conservative miss for allocations INSIDE
the closure, and it is orthogonal to registering the closure itself as a site.
Do not "fix" it in passing.

### Not tractable yet — interface boxing

`codegen_interface_box` has SIX-plus call sites (`expression_codegen.c` twice,
`composite_codegen.c` twice for struct fields and slice elements, `codegen.c`
for map keys, plus assignment and argument paths). Each decides locally by
comparing a target type against a value type.

**The AST carries no boxing marker** — `grep needs_box|is_boxed|iface_box
include/ast.h` returns nothing. So for `block_escape` to classify a box site it
would have to re-derive "this concrete value meets an interface slot" in every
one of those contexts, from the analysis side, without the type information
codegen has in hand.

That is the same shape this repository has paid for four times — several places
independently deciding one thing, then drifting (`codegen_coerce_arg_to_param`,
`codegen_emit_variadic_pack`, `codegen_emit_fixed_call_arg`, the
vendored-package fourth loop). Adding a fifth copy inside a soundness-critical
analysis is the worst possible place to repeat it.

**Prerequisite: give the boxing decision ONE owner.** Either the checker stamps
a marker on the node when it resolves concrete-meets-interface, or a single
predicate is shared by codegen and the analysis. Until that exists, an
interface-box site kind cannot be added soundly, only plausibly.

## Recommended order

1. **Closure and method-value environments.** Self-contained, the node is in
   hand, and the semantics reuse the existing taint engine unchanged. This is
   the piece to build first and the one that proves the widening approach.
2. **The boxing marker**, as its own change with its own review — one owner for
   "this value gets boxed", consumed by both codegen and `block_escape`.
3. **Interface boxing as a site kind**, once (2) exists.

## The test matrix any new site kind needs

Escape analysis cannot be validated by a green golden suite: over-marking is
invisible (it only costs memory) and under-marking is invisible until it
corrupts. Each new site kind needs, at minimum:

- **A non-escaping case** that measurably reclaims. RSS with the arena must
  fall well below RSS without it, via `/usr/bin/time -v` at a loop count high
  enough to dwarf noise. For interface boxing the RED is already recorded:
  400,000 boxed values, 13.9 MB with the arena against 14.5 MB without.
- **An escaping case per sink** that must still heap-allocate, and that READS
  the value after the arena block ends. A wrong `false` shows up here as
  garbage or a crash, and nowhere else. Sinks to cover: assignment to an outer
  local, return, a call argument that stores, a `go` statement, and storage
  into a longer-lived structure.
- **A mirror check against `param_escape.c`.** `block_escape.h` requires that a
  soundness fix to the shared taint-propagation shape be applied to both files.

## What was NOT established here

- No site kind was added, and no measurement improved. This pass establishes
  the ORDER and the prerequisite, nothing more.
- Phase 1 will not move the daemon number. The 651 MB at 400,000 requests lives
  in `append` and stdlib allocations, which never reach `codegen_emit_alloc` at
  all — that is phase 2, the runtime region. Phase 1 makes `arena` honest for
  the shapes it already claims to cover.
