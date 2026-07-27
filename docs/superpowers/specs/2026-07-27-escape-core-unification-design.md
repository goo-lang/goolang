# Escape-core unification (memory-reclamation program, Arc A) — design

**Date:** 2026-07-27
**Program:** memory reclamation, escape-driven, before any GC.
**This arc:** one taint engine instead of three hand-copies, plus the
allocation-site vocabulary the routing seam needs. No reclamation behavior
lands here — this is the foundation the stack-allocation and implicit-arena
arcs are built on.

## Why now

`include/block_escape.h` states the trigger condition itself:

> A soundness fix to that shared taint-propagation shape in ONE of these two
> files MUST be mirrored in the other (until/unless a third consumer justifies
> extracting a shared core).

Stack allocation is that third consumer: same engine, source = allocation
site, boundary = the enclosing **function**. The extraction the header defers
is now due.

There is also a **fourth escape mechanism already shipping and unaccounted
for**: `escape_is_promoted` (`src/codegen/function_codegen.c:162`) decides
goroutine/closure heap-promotion by matching variable **names**, through
file-static globals (`g_escape_has_go`, `g_escape_count`). It answers the same
question as the taint engine and shares no code with it. Four implementations
of "does this escape?" with nothing forcing agreement — the same drift shape
PRs #220/#224/#225 kept fixing in `call_codegen.c`, except here a disagreement
is a use-after-free rather than a bad diagnostic.

## Finding 1 — the engines have NOT rotted, and the divergence is load-bearing

Measured, not assumed: the taint engines were diffed function-for-function
(`param_escape.c:264-841` vs `block_escape.c:556-1135`, 578 vs 580 lines, 41
changed hunks). Nearly every hunk is comment rewording or a mechanical
coordinate rename (`param_count`↔`site_count`, `Registry`↔`ParamEscapeResult`).

**Exactly one behavioral divergence exists, and it is deliberate:**

| | `param_escape` (boundary = function) | `block_escape` (boundary = arena block) |
|---|---|---|
| `defer f(x)` | ordinary call — sink #5 only | **unconditional escape** — sink #4 |

`block_escape.c`'s own comment: *"This is the ONE place block-escape must
diverge from param_escape.c: at FUNCTION granularity a defer runs within the
frame... at BLOCK granularity the defer fires past the block boundary."*

**This is the extraction's principal trap.** A naive merge — diff the two,
pick one, delete the other — silently converts a deliberate boundary rule into
a use-after-free (arena-freed memory read by a deferred call) or into a
permanent pessimization. The defer rule MUST become an explicit policy input,
never a constant in the shared core.

## Finding 2 — five policy axes, and they vary independently

The diff decomposes cleanly into five parameters. The function-boundary
instantiation this arc adds is a **new combination** of them, which is the
strongest available evidence that this is the right factoring rather than an
over-fit to two existing callers:

| axis | `param_escape` | `block_escape` | **`function_escape` (new)** |
|---|---|---|---|
| bit-space `n` | param count | site count | site count |
| source seeding | LocalEnv seeded with params | starts EMPTY, sites discovered | starts EMPTY, sites discovered |
| self-taint injection | none (params are seeded) | `is_new_call`/`is_addr_of_composite` inject own bit | same as block |
| **defer** | stays inside boundary | **crosses** boundary | **stays inside** (a defer runs during frame teardown, frame still alive) |
| `return` | records `return_escapes` for the interprocedural fixpoint | plain escape | plain escape |

No axis is redundant, and the new column is not a copy of either existing one.

## Finding 3 (plan correction) — the site vocabulary is the real work

The program plan characterized "thread real `alloc_site` AST nodes through the
11 of 14 `codegen_emit_alloc` calls that pass NULL" as cheap and mechanical.
**It is necessary but not sufficient, and it is not mechanical.**

`block_escape` discovers exactly **two** allocation shapes —
`is_new_call` (`new(T)`) and `is_addr_of_composite` (`&T{}`),
`block_escape.c:183-196`. `find_site_index` returns `BLOCK_ESCAPE_NO_UNIT` for
anything else, and `codegen_arena_eligible`'s fail-safe contract turns that
into "heap". So threading an AST node through, say, the interface-boxing site
changes nothing at all until the discoverer also recognizes that shape.

The allocation kinds behind the 12 NULL-passing sites, each needing its own
discovery rule, escape reasoning, and test rows:

| kind | sites | note |
|---|---|---|
| interface boxing | `interface_codegen.c:661`, `codegen.c:736` | every box heap-copies; the highest-frequency leak in ordinary code (`fmt.Println(x)` in a loop) |
| closure environments | `composite_codegen.c:648`, `function_codegen.c:955`, `statement_codegen.c:2865` | already interacts with `escape_is_promoted`'s name-based path |
| slice/map backing | `composite_codegen.c:1691`, `codegen.c:1384`, `codegen.c:1405` | dynamically sized — Arc B's `alloca` cannot serve these; they are Arc C's case |
| misc / boxed values | `function_codegen.c:436`, `:450`, `statement_codegen.c:2400` | classify individually |

Consequence for sequencing: the vocabulary extension is its own task with its
own row matrix, **not** a step-3 afterthought. It is also the task that most
directly widens today's `arena { }` coverage, so the existing arena probes must
be re-read as behavior tests, not just regression tests.

## Task breakdown (revised)

1. **Extract the shared core**, parameterized on the five axes above. One
   module owning TaintSet, LocalEnv, `expr_taint`, `walk_stmt`,
   `assign_to_lvalue`, `call_taint`, `handle_go_call`, `handle_defer_call`,
   `seed_names_from_values`. Pure refactor: `param_escape` and `block_escape`
   become thin adapters supplying policy. **Gate: the 18-row param matrix and
   18-row block matrix pass unchanged, and emitted IR is byte-identical across
   the golden corpus.**
2. **Add the `function_escape` instantiation** on the core (the new policy
   column), with its own row matrix. No codegen consumer yet.
3. **Extend the site vocabulary** to the allocation kinds above, one kind per
   commit, each with discovery rule + rows. Re-verify the arena probes as
   behavior tests at each step.
4. **Thread `alloc_site`** through the 12 NULL-passing `codegen_emit_alloc`
   calls, matched to the vocabulary from (3).
5. **Subsume `escape_is_promoted`** into the core, keeping its current answers
   as a regression floor (`escape-probe`, `escape-range-probe` must not change
   behavior).

Tasks 1 and 2 are independent of 3-5 and can land first.

## Soundness discipline

- **`true` (escapes) is always the safe answer.** Over-marking costs
  performance; under-marking corrupts memory. Every construct the analysis does
  not precisely understand defaults to escaping — this is the review checklist
  item for every hunk in this arc.
- **The defer axis is checked explicitly in review.** See Finding 1.
- **ASan from task 3 onward.** Under-marking is invisible to the golden suite —
  a use-after-free usually still reads *plausible* memory and the fixture
  passes. `far-transport-asan` is clang-pinned here (gcc's libasan is broken on
  this box); reuse that pattern.
- **The existing 36 rows are the extraction's net.** They must pass unchanged,
  not be adjusted to fit the refactor. A row that needs editing is a signal the
  extraction changed semantics.

## Explicitly not in this arc

No `alloca` routing, no implicit arena, no reclamation of any kind, no GC. Arc
A ends with one engine, three instantiations, a full site vocabulary, and every
allocation site classifiable — and measurably identical behavior to today.
