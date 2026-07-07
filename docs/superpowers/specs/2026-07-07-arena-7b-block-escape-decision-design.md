# Arena leg — Task 7b: Per-Alloc-Site Block-Escape Decision (design)

Branch: `feat/arena-regions`. Depends on: 7a (banked `8a17857`, `ParamEscapeResult`).
Followed by: 7c (thread the decision into `codegen_emit_alloc`), 6 (arena free at block
exit — first point anything is freed), 8 (adversarial valgrind probe).

## Why this is safe to land now

Like 7a, 7b is **pure analysis with zero codegen wiring**. Arenas stay transparent/never-
freed (T5), so nothing here can promote, route, or free anything. It computes a per-site
decision and unit-tests it. The decision is only *consumed* in 7c.

## Relationship to 7a — the same engine, two coordinates moved

7b is 7a's taint analysis with:
- **source** = each *allocation site* inside an `arena {}` block (7a's source was a function
  parameter);
- **escape boundary** = the arena block (7a's boundary was the whole function).

The taint-propagation switch, the sink kinds, and the local fixpoint for loop back-edges are
identical in shape. 7b is implemented as a **new module `src/types/block_escape.{c,h}`**
that adapts `param_escape.c`'s engine; the banked 7a is left untouched. Both files carry a
"SOUNDNESS SIBLING" comment: a soundness fix to the shared taint-propagation shape in one
MUST be mirrored in the other (until/unless a third consumer justifies extracting a shared
core).

## What 7b computes

For every *allocation site* lexically inside an `arena {}` block, a boolean:

    escapes_block == true   ⇒  the allocated value may outlive the block ⇒ MUST be heap
    escapes_block == false  ⇒  provably dies with the block ⇒ arena-eligible

Same soundness invariant as 7a: **`true` is always safe.** Every construct not precisely
understood → `true` (heap). Under-marking (false when it truly escapes) is the UAF bug class
and must not happen.

### Allocation sites (the taint sources) — first cut

A site is one of these expression nodes, lexically within an `arena {}` block, analyzed
relative to its **innermost** enclosing arena block:

1. `new(T)` — `AST_CALL_EXPR` whose `function` is the identifier `new`.
2. `&<composite literal>` — address-of a struct/array/slice literal (the `&Node{}` /
   `&T{...}` form; confirm the exact node kind against the AST before coding — see
   `expression_codegen.c:2382`).

Every OTHER implicit allocation the codebase funnels through `codegen_emit_alloc` (interface
boxing, map boxing, closure env, `go`-arg boxing, slice-literal backing — the 11 choke-point
callers) is **out of scope for this cut and conservatively stays heap**: 7b simply does not
classify those nodes as sites, so 7c finds no decision for them → miss → heap. This is safe;
it only forgoes the arena benefit for those forms. Document, do not expand here.

### Escape boundary — block scope

While analyzing an arena block B, a variable name is **block-local** iff it is declared
textually within B (via `var`/`:=`/range/if-let/type-switch binding at any nesting inside B).
A variable declared *before* B in the same function, a function parameter, or a package
global is **outside** B. A value declared within B cannot outlive B, so assignment to a
block-local is *propagation*; assignment to anything outside B is a *store escape*.

Nested arena blocks: each site belongs to its innermost enclosing arena block; a site whose
value flows to a variable declared in an *enclosing* arena block (outside the innermost) is
treated as escaping the innermost block (conservative — promote to heap, never to the outer
arena in this cut).

## Escape sinks (a site escapes its block if a value tainted with it reaches any)

Identical set to 7a, with the boundary reframed to the block:

1. **Return** — `return e`, `e` tainted with the site.
2. **Store to a non-block-local location** — assignment `lhs = rhs` (or `op=`), `rhs`
   tainted, and `lhs` is anything other than a plain block-local of B: an outer-scope
   variable, a global, `*p`, `obj.field`, `arr[k]`, etc.
3. **Closure capture** — a `FuncLitNode` whose `captured_names[]` contains a block-local
   tainted with the site. (Bodies not walked — relies on the checker's capture analysis,
   same precondition as 7a: run AFTER `type_check_program`.)
4. **Goroutine / deferred call** — `go G(...)` OR `defer G(...)`: every argument
   tainted with the site escapes unconditionally. A goroutine may outlive the block;
   a `defer` runs at the enclosing *function's* exit, which is always after the arena
   block frees its arena — so a deferred call's (defer-time-snapshotted) arguments also
   outlive the block. This is the one sink where block-escape must diverge from
   param-escape: at function granularity a `defer` runs within the frame (param-escape
   correctly treats it as an ordinary call), but at block granularity it fires past the
   block boundary and must escape. (Regression: `examples/arena_defer_escape_probe.goo`,
   block_escape_test row 16.)
5. **Retaining call argument** — call `G(a_0…a_m)`, `a_k` tainted, and position k retains:
   `summaries` says `G.escapes[k]` (via `param_escape_param_escapes`), OR G is
   external/unregistered/selector/body-less → **all positions retain** (pure-conservative,
   consistent with 7a). Spread/variadic tail → retain.

Anything unhandled → conservative escape of every site mentioned within it.

## Intraprocedural taint (per arena block)

Bitset width = number of sites in the block. Seed: when the taint walk evaluates an
allocation-site node, it yields a taint with that site's bit set. A `local name → taint set`
map propagates through `:=`/`var`/assignment exactly as in 7a (monotone union, never clears).
Iterate the block-body walk to a local fixpoint (loop back-edges). A site escapes iff its bit
reaches any sink above.

## Consuming 7a

`block_escape_analyze` takes the `ParamEscapeResult` by parameter (dependency injection; 7c
computes summaries once and passes them to 7b). Retaining-call decisions use
`param_escape_param_escapes(summaries, fn, k)` — which already returns `true` on unknown-fn /
out-of-range, so the external-retains-all rule falls out for free.

## Public API

```c
typedef struct BlockEscapeDecision {
    ASTNode* site;          // the alloc-site node (new-call or &composite)
    bool     escapes_block; // true = must heap; false = arena-eligible
} BlockEscapeDecision;

typedef struct BlockEscapeResult {
    BlockEscapeDecision* decisions; // one per site inside an arena block, source order
    size_t count;
} BlockEscapeResult;

// Pure; does not mutate `program`. `summaries` may be NULL (then every call is treated
// as external/retaining — still sound). NULL return only on allocation failure.
BlockEscapeResult* block_escape_analyze(ASTNode* program, const ParamEscapeResult* summaries);
void block_escape_result_free(BlockEscapeResult*);

// 7c's query: does the value produced at `site` escape its arena block?
// TRUE on a miss (site unknown / not classified) — conservative: 7c then keeps it on the
// heap/default path. This miss-behaviour is part of the soundness contract.
bool block_escape_site_escapes(const BlockEscapeResult*, const ASTNode* site);
```

`decisions` is in pre-order source traversal order so a table test can assert by index.

## Test matrix (RED first — write before the implementation)

C unit test `tests/unit/types/block_escape_test.c`, table-driven: Goo source → `parse_input`
→ `type_check_program` (populate captures) → `param_escape_analyze` → `block_escape_analyze`
→ assert `decisions[i].escapes_block`. Link `SRC_OBJS`. Make target `block-escape-test`,
wired into `verify` and `.PHONY`/`clean-tests`/`.gitignore` like `param-escape-test`.

Validate each source PARSES first; rewrite to a parsing equivalent preserving escape
semantics if a construct is unsupported, and report any rewrite/skip.

| # | Case (inside `arena { }`) | Expected |
|---|---------------------------|----------|
| 1 | `x := new(int); _ = x` (dies in block) | **false** (arena-eligible) |
| 2 | `func f() *int { arena { return new(int) } }` | **true** |
| 3 | outer `var keep *int` before block; `keep = new(int)` in block | **true** |
| 4 | package `var g *int`; `g = new(int)` in block | **true** |
| 5 | `sink(x *int){}`; `p := new(int); sink(p)` (non-retaining callee) | **false** |
| 6 | `var g; stash(x){ g=x }`; `stash(new(int))` (retaining callee) | **true** |
| 7 | `fmt.Println(new(int))` (external retains) | **true** |
| 8 | `p := new(int); go func(){ use(p) }()` (closure capture) | **true** |
| 9 | `g(x *int){}`; `go g(new(int))` (goroutine arg) | **true** |
| 10 | `func f() *int { arena { a := new(int); b := new(int); _ = a; return b } }` | a=**false**, b=**true** |
| 11 | `func f() *int { arena { x := new(int); y := x; return y } }` (through local) | **true** |
| 12 | `func f(out **int){ arena { *out = new(int) } }` (store through param ptr) | **true** |
| 13 | `n := &Node{}; m := &Node{next: n}; _ = m` (block-local aggregate, both die) | both **false** |
| 14 | site OUTSIDE any arena block (`x := new(int)` at top of func, no arena) | not recorded; `block_escape_site_escapes` → **true** on miss |
| 15 | `p := new(int); if cond { keep = p }` where `keep` is outer var (conditional escape) | **true** |

Must-be-**false** rows (1, 5, 13, and `a` in 10) prove the arena actually delivers — a lazy
mark-everything impl fails them. Must-be-**true** rows (2,3,4,6,7,8,9,11,12,15, and `b` in
10) are the soundness rows — an unsound impl shows up as a wrong **false**.

## Explicitly NOT in 7b

- No codegen change (that is 7c). `codegen_emit_alloc` untouched; no arena freed; no
  promotion. Arenas stay transparent.
- Only `new(T)` and `&composite` are classified as sites this cut; all other implicit
  `codegen_emit_alloc` forms stay heap (conservative).
- Loop-body per-iteration arena scopes remain out of scope (plan's stated limitation).
- The banked `param_escape.c` is not modified.
