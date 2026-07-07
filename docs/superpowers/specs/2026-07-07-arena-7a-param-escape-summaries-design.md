# Arena leg — Task 7a: Interprocedural Param-Escape Summaries (design)

Branch: `feat/arena-regions`. Depends on: T1–T5 (banked). Followed by: 7b (per-alloc-site
block-escape decision), 7c (thread decision into `codegen_emit_alloc`), 6 (arena free at
block exit), 8 (adversarial valgrind probe).

## Why this is safe to land now

7a is **pure analysis with zero codegen wiring**. Arenas remain transparent/never-freed
(T5), so nothing this task produces can promote an allocation, free an arena, or dangle a
pointer. It computes summaries and unit-tests them; that is all. The soundness stakes are
real (an unsound summary consumed by a *later* task = silent use-after-free), so the
summaries themselves are verified adversarially here, before any consumer exists.

## What 7a computes

For every user-defined function `F`, a **param-escape summary**: one boolean per parameter
*name*,

    escapes[i] == true  ⇒  a value reachable from parameter i may outlive the call to F

plus one boolean `return_escapes` used only as an internal signal during the fixpoint
(does F return a value derived from one of its params — needed so a caller passing an arg
into a returning position sees that arg escape).

**Soundness invariant (the whole point):** `true` is always safe (worst case: promote to
heap, lose the arena benefit). `false` asserts "provably does not outlive." Therefore every
construct we cannot precisely analyse defaults to `true`. Under-marking is the only bug
class that can dangle; over-marking merely costs performance.

### Parameter *names*, not param nodes

A parameter list is a chain of `VarDeclNode`, and one node may declare several names
(`func f(a, b int)` = one `VarDeclNode` with `name_count == 2`). The summary is indexed by
flattened parameter-*name* position (a→0, b→1), NOT by `VarDeclNode`. The old
`escape_analysis.c` counted nodes; that is wrong here and must not be copied.

## Escape sinks (a param escapes if a value tainted with it reaches any of these)

1. **Return** — `return e` where `e` is tainted with param i. (Also sets `return_escapes`.)
2. **Store to a non-local location** — assignment `lhs = rhs` (or `lhs op= rhs`) where
   `rhs` is tainted with i and `lhs` is anything other than a plain local of F: a global,
   `*p` through a pointer, `obj.field`/`arr[k]` where the base is non-local, or a variable
   declared in an enclosing scope. (Assignment to a plain local of F is *propagation*, not
   escape — see taint rules.)
3. **Closure capture** — a `FuncLitNode` whose `captured_names[]` contains a name tainted
   with i. (`captured_names` is populated by the checker; we do not re-walk the closure
   body.) Captured ⇒ the value may outlive F.
4. **Goroutine** — a `go G(...)` statement: every argument tainted with i escapes
   (the goroutine may outlive F), independent of G's summary.
5. **Retaining call argument** — a call `G(a_0, …, a_m)` where argument `a_k` is tainted
   with i AND position k of G retains. "Retains" =
   - G is a user function with a summary: `G.escapes[k] == true`; OR
   - **G is body-less / external / unregistered (stdlib, builtins, method calls we do not
     resolve): ALL positions retain** (pure-conservative — per the 7a decision; the
     non-retaining whitelist is deferred to 7a′).
   - `has_spread` / variadic tail: treat every packed arg as retaining unless every covered
     position is known-non-retaining (i.e. conservative = retain).

Anything not enumerated (unhandled statement/expression type reachable from a param) →
conservative escape of every param mentioned within it.

## Intraprocedural taint

Per function, a map `local name → set of param indices it may alias`. Seed each parameter
name i with `{i}`. Taint of an expression `taint(e)`:

- identifier x → the set stored for x (params seeded; unknown/global → ∅, but see sink #2:
  a global appearing as an *lhs* is a sink regardless of its taint).
- `&e`, `*e`, `e.f`, `e[k]`, unary/postfix on e → `taint(e)` (address-of and deref both
  carry; field/index of a tainted aggregate is conservatively tainted).
- binary `a op b` → `taint(a) ∪ taint(b)`.
- call `G(args…)` result → if G returns an arg-derived value (`G.return_escapes`), union of
  the taints of every arg in a returning position; for external G, conservatively
  `⋃ taint(arg_k)` over all args. (Sound over-approx: a callee may return any arg.)
- composite literals `T{…}`, slice/map literals → union of element taints.
- literals, `nil`, type expressions → ∅.

**Flow:** a single forward pass is unsound under back-edges (loops) and later definitions.
The taint map only grows (monotone), so iterate the body walk to a **local fixpoint**
(repeat until the map stops changing; bounded by #locals × #params). Assignment
`local = rhs` unions `taint(rhs)` into `taint(local)` (never removes — SSA-free, so we must
assume all reaching defs). `:=` / var-decl with initializer seeds the new local's taint.

## Interprocedural fixpoint

Lattice: per (function, param) boolean, `false ⊑ true`; plus per-function `return_escapes`.
All monotone (only `false → true`). Algorithm:

1. Register every `AST_FUNC_DECL` (methods too; receiver is param name 0 — it is spliced as
   the head of `params`). Record param-name count per function.
2. Initialize all `escapes[*] = false`, `return_escapes = false`.
3. Repeat until a full pass makes no change: for each F, recompute its summary using the
   **current** summaries of its callees (external/unknown callees use the retain-all rule).
4. Converges because the state space is finite and strictly monotone; recursion and mutual
   recursion are handled by the fixpoint (a self/mutual call reads the growing summary).
   A safety cap on iteration count is a bug-catcher, not the termination argument; if the
   cap is ever hit, fail *closed* (mark all remaining params escaping).

## Public API (new module)

New: `src/types/param_escape.c`, `include/param_escape.h`. Reuses `EscapeKind` vocabulary
from `memory_safety.h` (ESCAPE_NONE vs escaping) but NOT the hollow `EscapeAnalyzer`.

```c
typedef struct ParamEscapeSummary {
    char*  function_name;   // owned
    bool*  escapes;         // length param_name_count; true = may outlive the call
    size_t param_count;     // number of parameter NAMES (flattened)
    bool   return_escapes;  // internal fixpoint signal
} ParamEscapeSummary;

typedef struct ParamEscapeResult {
    ParamEscapeSummary* summaries;   // one per user function
    size_t count;
} ParamEscapeResult;

// Pure. Does not mutate `program`. NULL on allocation failure only.
ParamEscapeResult* param_escape_analyze(ASTNode* program);
void param_escape_result_free(ParamEscapeResult*);

// Lookup helpers (return NULL / true-on-miss where a miss must be conservative).
const ParamEscapeSummary* param_escape_lookup(const ParamEscapeResult*, const char* fn);
bool param_escape_param_escapes(const ParamEscapeResult*, const char* fn, size_t param_idx);
```

`param_escape_param_escapes` returns **true** for an unknown function or out-of-range index
(conservative miss). This is the interface later tasks call; its miss-behaviour is part of
the soundness contract.

## Test matrix (RED first — write these before the implementation)

C unit test `tests/unit/types/param_escape_test.c`, table-driven. Each row: Goo source
string → `parse_input` → `param_escape_analyze` → assert `escapes[]` for named functions.
Link against `SRC_OBJS` (parser + lexer + ast + types). Make target `param-escape-test`,
wired into a `test-units`-style phony and into `verify` prerequisites.

| # | Case | Function / param | Expected |
|---|------|------------------|----------|
| 1 | param unused | `func f(p *T){ }` p | **false** |
| 2 | param only read/printed via **external** call | `func f(p *T){ fmt.Println(p) }` p | **true** (external retains — pure-conservative) |
| 3 | param returned | `func f(p *T) *T { return p }` p | **true**, `return_escapes` true |
| 4 | one of two returned | `func f(a, b *T) *T { return b }` a,b | a=false, b=**true** |
| 5 | stored to global | `var g *T; func f(p *T){ g = p }` p | **true** |
| 6 | stored to plain local, never out | `func f(p *T){ x := p; _ = x }` p | **false** |
| 7 | captured by closure | `func f(p *T){ go func(){ use(p) }() }` (captured_names has p) p | **true** |
| 8 | passed to goroutine arg | `func f(p *T){ go g(p) }` p | **true** |
| 9 | passed to **non-retaining** user callee | `func g(x *T){ } func f(p *T){ g(p) }` p | **false** (g doesn't retain) |
| 10 | passed to **retaining** user callee | `var g *T; func stash(x *T){ g = x } func f(p *T){ stash(p) }` p | **true** (transitive) |
| 11 | recursion terminates | `func f(p *T){ f(p) }` p | terminates; p=false (self-call non-retaining) |
| 12 | mutual recursion terminates | `func a(p *T){ b(p) } func b(q *T){ a(q) }` | terminates; both false |
| 13 | transitive-through-return | `func id(x *T) *T { return x } func f(p *T) *T { return id(p) }` p | **true** |
| 14 | field store on param (out-param) | `func f(p *Box){ p.next = p }` p | **true** (store through param) |
| 15 | method receiver as param 0 | `func (r *T) m(){ g = r }` (r is param 0) | r=**true** |

Cases 2, 5, 7, 8, 10, 13, 14 are the load-bearing "must be true" rows — an unsound
implementation shows up as a **false** here. Cases 1, 6, 9 guard against trivial
mark-everything (which would be sound but useless); they must be **false**.

## Explicitly NOT in 7a

- No codegen change; `codegen_emit_alloc` is untouched. No arena is freed. No promotion.
- No non-retaining whitelist (deferred → 7a′, dedicated adversarial review).
- No per-alloc-site block decision (that is 7b, which consumes these summaries).
- The old `src/types/escape_analysis.c` is left dead and untouched (deletable later).
