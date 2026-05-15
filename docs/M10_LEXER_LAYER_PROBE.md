# M10 lexer-layer strategy probe (2026-05-15)

Design-only assessment of Strategy 2 from `docs/M10_COMPOSITE_LITERAL_SPIKE_2.md`:
a **lexer-layer context tracker** that decides at token-emit time whether an `{`
opens a cond-following block (`LBRACE_BODY`) or a regular brace (`LBRACE`, which
also opens a composite literal). No parser mid-rule actions are involved.

Source files inspected:
- `src/parser/parser.y` (2040 LOC) — all `expression block` patterns
- `src/parser/lexer_bridge.c` (224 LOC) — token translation layer
- `src/lexer/lexer.c` (581 LOC) — raw token producer (stateless, kept that way)
- `docs/M10_COMPOSITE_LITERAL_SPIKE_2.md`, `docs/M10_BASELINE_CONFLICTS_AUDIT.md`

## TL;DR

- **Viable with caveats.** The lexer bridge has the right shape: `yylex()` already
  funnels every token, owns the bison-token mapping, and has a natural home for
  per-parse state. We do not need to touch `lexer.c` at all — the bridge is the
  single chokepoint.
- **LOC estimate: ~110–160 LOC total.** Bridge ~80–120 LOC (frame stack +
  push/pop logic), parser.y ~10–20 LOC (`LBRACE_BODY` token decl plus 4–6 rules
  that accept either brace token), no `lexer.c` changes.
- **Scariest edge case: `guard_condition: IF expression` inside match_case
  (parser.y:1855–1860).** A guard `IF` terminates with `COLON`, not `LBRACE`.
  A naive "push IF_COND on IF, pop on depth-0 LBRACE" leaks the frame, so the
  *next* statement's first brace at the leaked depth will be wrongly recoloured
  `LBRACE_BODY`. The state machine must pop the IF_COND frame on `COLON` at
  depth-0 too, or admit that guard-IFs are lexically indistinguishable.
- **Second-scariest: `catch_expr: expression CATCH identifier block` (parser.y:1446).**
  A block can appear mid-expression with no cond preceding it; worse, it can
  nest inside an IF_COND frame (`if x catch err { recover } { body }` has two
  depth-0 braces — the first closes the catch, the second is the if's block).
  The frame must survive the first brace.
- **`match_expr` (parser.y:1777) is the same shape as IF and needs the same
  treatment.** A MATCH frame must also recolour the next depth-0 `{` to
  `LBRACE_BODY` even though the body is `match_case_list`, not `block`.

## State machine design

### Frame kinds

```
enum CondFrame {
    IF_COND,         // pushed on IF (incl. guard-IF inside match_case)
    FOR_COND,        // pushed on FOR
    MATCH_COND,      // pushed on MATCH
    SWITCH_COND,     // future: SWITCH token exists, no switch_stmt rule yet
    SELECT_COND,     // pushed on SELECT (the `{` immediately follows SELECT)
}
```

Each frame stores:
- `kind` (one of the above)
- `paren_depth_at_push` (snapshot of the current paren+bracket counter so we
  can detect "back to the depth we were at when pushed" — necessary because
  parens can intervene: `if (x + y) > 0 { body }`).

### Global state (per parse, in `lexer_bridge.c`)

```
static CondFrame frame_stack[MAX_FRAMES];   // depth ~32 is overkill
static int       frame_top = -1;            // empty when -1
static int       paren_depth = 0;           // counts LPAREN+LBRACKET nesting
static int       brace_depth = 0;           // counts LBRACE nesting (post-translation)
```

`MAX_FRAMES` of 64 covers any realistic nesting (recursion through
`if(if(if...))` would have to be deep enough to hit it).

### Transition rules (pseudocode, runs inside `yylex()` after `map_token_to_bison`)

```
on emit IF:
    push_frame(IF_COND, paren_depth)

on emit FOR:
    push_frame(FOR_COND, paren_depth)

on emit MATCH:
    push_frame(MATCH_COND, paren_depth)

on emit SELECT:
    push_frame(SELECT_COND, paren_depth)

on emit SWITCH:                     // forward-compat
    push_frame(SWITCH_COND, paren_depth)

on emit LPAREN | LBRACKET:
    paren_depth++

on emit RPAREN | RBRACKET:
    paren_depth--                   // negative is a parse error; pass through

on encountering '{' (BEFORE returning the token):
    if frame_top >= 0
       AND frame_stack[frame_top].paren_depth_at_push == paren_depth
       AND brace_depth == 0_relative_to_frame:        // see "depth tracking"
        pop_frame()
        brace_depth++
        return LBRACE_BODY
    else:
        brace_depth++
        return LBRACE

on emit RBRACE:
    brace_depth--

on emit COLON at paren_depth == frame.paren_depth_at_push
   AND frame.kind == IF_COND:
    pop_frame()                     // guard-IF terminator; see edge case 4

on emit SEMICOLON inside FOR_COND at frame's paren_depth:
    // c-style FOR: `for init; cond; post { body }`. SEMICOLONs do NOT close the
    // frame — the FOR_COND remains pinned until the next depth-matched '{'.
    no-op
```

### Depth tracking semantics

The key invariant: **a `{` is `LBRACE_BODY` iff the topmost frame's
`paren_depth_at_push` matches the current `paren_depth` AND no intermediate
`{` is still open between the cond keyword and here.**

The `paren_depth_at_push` check is what distinguishes the if's body brace from a
brace nested inside parens or brackets:

- `if X { body }` — IF pushed at paren_depth=0, `{` seen at paren_depth=0 → BODY
- `if foo(Point{x:1}) > 0 { body }` — IF at depth 0, `(` lifts depth, `{` of the
  composite literal is at depth 1 → stays LBRACE → `)` returns depth to 0 → next
  `{` is at depth 0 with the frame still on top → BODY
- `arr[Point{x:1}]` outside any IF — frame_top is -1, all `{` are LBRACE

We do not actually need a separate `brace_depth` for *deciding* (the
paren_depth_at_push check suffices) — but tracking `brace_depth` is helpful for
debugging and for the panic recovery path.

### Lifecycle integration

`parse_input` (lexer_bridge.c:181) is the entry point. It already creates and
frees the lexer. Add:

```
parse_input():
    frame_top = -1
    paren_depth = 0
    brace_depth = 0
    current_lexer = lexer_new(...)
    result = yyparse()
    // do NOT assert frame_top == -1 here — parser errors can leak frames;
    // we reset on next call.
    lexer_free(current_lexer)
```

This means there's no need to wire `parser_init`/`parser_cleanup` (those are
called from `src/ide/lsp_enhanced.c:112`) into the state — reset-on-entry is
sufficient. `parse_input` is the only public driver.

## Edge cases

### 1. Nested if in an expression: `if foo(if x { 1 } else { 2 }) > 0 { body }`

Goolang's grammar does **not** allow `if` as an expression (`expression` derives
through `unary_expr`/`primary_expr` with no IF alternative). So this case
cannot occur in valid Goolang. If it did, the design handles it: inner IF pushed
at paren_depth=1 (inside `foo(...)`), inner `{` at depth 1 matches inner frame
→ LBRACE_BODY, pop. Outer IF still on stack at depth 0; outer `{` at depth 0
→ LBRACE_BODY.

### 2. Anonymous func inside a cond: `if (func() int { return 1 })() > 0 { body }`

Goolang has `func_type: FUNC func_signature` (parser.y:1380) — function types
exist as types, but a closure-as-expression form is NOT in `primary_expr`. So
this also can't currently occur. If/when it's added, the design handles it
identically to case 1: the closure-body `{` would land at paren_depth ≥ 1
(inside the `()`), so it stays `LBRACE`. To make it a *block*, the parser
would need closure-body grammar that accepts `LBRACE | LBRACE_BODY` — or the
frame design could push a `FUNC_BODY` frame on FUNC-as-expression-context.
Out of scope for M10.

### 3. try/match inside conds: `if try foo() > 0 { body }`

`try_expr: TRY expression` (parser.y:1438) is a prefix expression that doesn't
introduce braces. No frame interaction; depth stays 0; if body brace gets
LBRACE_BODY correctly.

`if match x { 1: y, 2: z } > 0 { body }` is more interesting: MATCH pushes a
MATCH_COND frame, the inner `{` at depth 0 matches the MATCH frame → LBRACE_BODY
emitted for the match body, frame popped. Outer if's `{` then matches the
remaining IF_COND frame → LBRACE_BODY. Both work, **provided the parser accepts
`match_expr: MATCH expression LBRACE_BODY match_case_list RBRACE`** — see LOC.

### 4. **Guard condition in match_case: `case Foo if x > 0: stmt`** (THE SCARIEST)

`guard_condition: IF expression` (parser.y:1855–1860) terminates with `COLON`,
NOT a brace. Naive push-on-IF/pop-on-LBRACE leaks the frame. The next statement
(in the case body) would have its first depth-0 `{` recoloured as LBRACE_BODY,
breaking composite literals inside case bodies.

**Mitigation:** add a transition rule "pop topmost IF_COND on COLON at the
frame's paren_depth_at_push." This is safe because a depth-0 COLON inside a
normal-if's cond is itself a parse error (Goolang has no ternary), so the only
legitimate IF...COLON path is the guard form.

Residual risk: typed pattern bindings use COLON (`identifier COLON type` in
patterns, parser.y:1835). But those only appear inside `pattern`, which is
inside `match_case`, which is inside `match_expr`'s body — by which point the
MATCH_COND has already been popped (we popped on the match body's LBRACE).
A pattern's COLON happens with the topmost frame being either none, or an
outer (unrelated) cond at a different paren_depth — so the pop condition won't
fire. Confirmed safe.

### 5. **`catch_expr: expression CATCH identifier block` mid-expression**

`x catch err { recover() }` can appear inside an if cond:
`if x catch err { recover() } { body }`. The first `{` belongs to the catch's
block; the second is the if's body.

With our design: IF pushes frame at paren_depth=0. The first `{` is at
paren_depth=0 with IF_COND on top — we'd emit LBRACE_BODY and pop. WRONG: we
want the catch block to be a normal block-following-expression that the parser
sees as LBRACE.

**Mitigation option A (simpler):** Catch's block accepts both `LBRACE` and
`LBRACE_BODY`. The parser still gets a valid parse (catch eats LBRACE_BODY,
moves on). But now the IF_COND frame is popped — when we hit the *real* if
body's `{`, frame_top is empty and we emit plain `LBRACE`. The parser sees
`IF expression LBRACE ... RBRACE` — does this still parse? **Yes**, because
the existing `block: LBRACE statement_list RBRACE` rule is unchanged and the
`if_stmt: IF expression block` rule will accept it. The LBRACE_BODY route is
an *optimization* to disambiguate against struct_lit-in-cond; if catch
consumes the disambiguation token early, the if-body brace simply takes the
old (struct-lit-ambiguous) path. But struct_lit is the thing we're trying to
allow in conds — and after `catch identifier`, the IDENT.LBRACE that would
have been a struct_lit has already been disambiguated by the CATCH keyword,
so there's no remaining ambiguity. Net: **catch in cond works, with the
LBRACE_BODY benefit applying to whichever brace comes first.**

**Mitigation option B (more correct):** Re-push a fresh IF_COND frame when
catch's block closes. Requires the bridge to track "we're inside a catch
that's inside an IF_COND" — adds complexity. Defer unless option A breaks in
practice.

Recommendation: ship option A. Catch-in-cond is rare; the partial
disambiguation is sufficient.

### 6. Anonymous struct literal inside `for`-cond: `for x := range []int{1,2,3} { body }`

The `[]int{...}` is a slice literal (Goolang's `slice_lit`, parser.y:1073)
already accepted in `primary_expr`. After FOR pushes FOR_COND, the
`{` of the slice lit appears at paren_depth=0 (LBRACKET also raises
paren_depth in our design — verify) → we'd wrongly call it LBRACE_BODY.

**Resolution:** treat LBRACKET like LPAREN: it raises `paren_depth`. So
`[]int{1,2,3}` opens at depth 1 (after `[`), and the slice lit's `{` is at
depth 1, not depth 0. The FOR_COND frame stays alive until the depth returns
to 0 at the for body's `{`. **The design above already has this — re-stated
here because it's load-bearing.**

### 7. Unmatched RBRACE / parser error mid-cond

If yyparse hits an error mid-cond and aborts, `frame_top` and `paren_depth`
will be non-zero on return. We reset them at the next `parse_input` entry, so
the leak is contained to a single parse. The error message itself is unaffected
(the parser doesn't query the frame stack).

For long-running LSP use (`lsp_enhanced.c`): `parser_init`/`parser_cleanup`
do exist but are LSP-side. To be safe, expose a `bridge_reset_cond_state()`
function and call it from `parser_init`. ~5 LOC.

### 8. select_stmt: `SELECT LBRACE select_case_list RBRACE` (parser.y:910)

The `{` immediately follows SELECT — no expression in between. We still push
SELECT_COND on SELECT, then immediately pop on the `{`, emitting LBRACE_BODY.
The parser rule must change from `SELECT LBRACE` to `SELECT LBRACE_BODY`. Trivial.

### 9. parallel_for_stmt (parser.y:1536–1537)

`PARALLEL FOR identifier ... expression block` — the FOR token *does* appear
here, so FOR_COND gets pushed. The expression preceding the block is at
paren_depth=0. The depth-0 `{` after it matches the FOR_COND frame →
LBRACE_BODY. **Same treatment as for_stmt; no extra changes.** (PARALLEL by
itself doesn't push a frame.)

## LOC estimate

| File | Δ LOC | Justification |
|---|---|---|
| `src/parser/lexer_bridge.c` | +80 to +120 | Frame stack (struct + storage, ~10), push/pop helpers (~15), transition rules inlined into `yylex` per-token (~30), reset on `parse_input` entry (~5), `bridge_reset_cond_state` for LSP (~5), comments documenting invariants (~15–55). |
| `src/parser/parser.y` | +10 to +20 | `%token LBRACE_BODY` (1 line), `block` accepts both (3 lines: new alternative), `match_expr` accepts both for its `{` (3 lines), `select_stmt` switches LBRACE→LBRACE_BODY (1 line), `catch_expr` accepts both (3 lines), `comptime_block`/`unsafe_stmt`/`if_let_stmt`/`asm_stmt` need audit — likely covered by `block`-alternative since they delegate to `block` (0 lines), comments (~5 lines). |
| `src/lexer/lexer.c` | 0 | Untouched. Stateless lexer preserved for IDE/LSP reuse. |
| `include/parser.h` | +1 to +3 | Declare `bridge_reset_cond_state()`. Optional. |
| **Total** | **~95 to ~145 LOC** | |

The spike doc's "~5 LOC parser.y" estimate is too low — it counts only the
`block` rule. Once `match_expr`, `select_stmt`, and `catch_expr` are audited,
the realistic delta is ~10–20.

## Risk assessment

### Failure modes

1. **Frame stack desync** (most likely failure). A frame gets pushed and never
   popped because the corresponding `{` never arrives (parse error, missing
   brace in source). Mitigation: reset on parse entry (cheap, already designed).
   **Impact: contained to a single parse.**
2. **Wrong LBRACE_BODY emission inside catch_expr nested in cond** (edge case 5).
   Mitigation: grammar accepts both token kinds; the parser stays correct even
   if the lexer "wastes" a LBRACE_BODY. **Impact: zero functional regression;
   loss of disambiguation benefit in a corner case.**
3. **paren_depth underflow on unmatched `)` or `]`** (malformed input). Becomes
   negative. The push/pop arithmetic still works — frames pushed at depth 0 stay
   active. Defensive cap at zero is a one-liner. **Impact: cosmetic.**
4. **MATCH_COND interacts with future SWITCH** when `switch_stmt` is added.
   Mitigation: SWITCH_COND already designed in. **Impact: zero, by construction.**

### Testability

The state machine is **highly testable in isolation**:
- The bridge's frame stack + transition rules can be unit-tested by feeding
  synthetic token sequences (no real source needed) and asserting the
  recoloured token stream.
- A dedicated test file `tests/parser/test_lexer_bridge_context.c` can cover
  each edge case above without invoking yyparse.
- Existing baseline_probe.goo + smoke-stdlib serve as integration tests for the
  parse path.

This is a strict improvement over the mid-rule-action approach, which could
only be tested through full yyparse runs with Bison's internal state visible
only via `parser.output`.

### Lifecycle integration

- Single entry point (`parse_input`) makes reset trivial.
- LSP path (`parser_init`/`parser_cleanup` in `lsp_enhanced.c`) needs a single
  call into `bridge_reset_cond_state()` to ensure the long-lived LSP server
  doesn't leak frames across requests. ~3 LOC change in lsp_enhanced.c.
- No interaction with Bison's `%pure-parser` or thread-safety concerns —
  the existing code is already single-threaded (`current_lexer` is a global).

## Verdict on strategy 2 (lexer-layer)

**Viable with caveats.** Concretely:

- The bridge already owns every yylex emission, so we don't need to touch
  `lexer.c`. The state machine fits naturally inside the existing
  `map_token_to_bison` + `yylex` flow.
- Total LOC delta is ~95–145, dominated by the bridge changes. parser.y delta
  is small (~10–20 LOC) because we add one token and selectively accept it in
  ~5 rules.
- The approach is **orthogonal to the 217 baseline conflicts** (per the
  baseline audit, doc 3): it adds one new token, no new rules in `primary_expr`
  or the operator cascade, so it cannot perturb the unary-vs-binary clusters
  (A) or the extend-or-stop clusters (C/D/E) that hold 90% of the baseline mass.
- The previous mid-rule-action attempt failed because mid-rule actions create
  epsilon productions that interact with Bison's reduce decisions in the
  high-traffic states. Our approach creates **zero new productions on the
  expression path** — `block`'s new alternative is the only addition, and it's
  on a path that doesn't touch the conflict-heavy states.

**Caveats:**

1. The **guard_condition IF** edge case (case 4) requires a COLON-triggered
   pop. This is non-obvious; future maintainers must understand it. Document
   in the bridge source with a pointer to this doc.
2. **catch_expr inside cond** (case 5) gets partial disambiguation only. The
   simpler resolution (both tokens accepted in catch's block rule) loses the
   benefit but doesn't break. Document the trade-off.
3. The **lexer becomes context-sensitive** in a small, well-bounded way. This
   is the unavoidable cost of the strategy. The cost is contained to
   `lexer_bridge.c` — `lexer.c` stays pure.
4. **MATCH_COND must be added now** even though composite-literal-in-MATCH-cond
   is rare, because match_expr has the same IDENT.LBRACE shape as IF. Skipping
   it means MATCH stays ambiguous and `match Point{x:1} { ... }` still fails.

Net recommendation: **commit to strategy 2.** It's the smallest path that
solves the underlying issue without (a) the GLR rewrite cost or (b) the
language-design migration cost. The risks above are bounded and addressable.
