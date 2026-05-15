# M10 composite-literal spike, round 2 (2026-05-15)

Follow-up to `docs/M10_COMPOSITE_LITERAL_SPIKE.md`. The original spike recommended Resolution A (parallel `expression_nc` hierarchy). This document records the empirical finding that **A does not work in the real Goolang grammar**, evaluates B/C/D in more depth, and recommends a different path.

## TL;DR

- **Resolution A fails in production grammar.** The minimal-reproduction test Bison-validated to zero conflicts; adding the parallel hierarchy to the real `parser.y` produced **386 reduce/reduce conflicts**. Root cause: in the real grammar, `expression` and `expression_nc` both derive from leaves (identifier, literal, primary_expr) through long recursive cascades. Bison's LR(1) reductions at the leaves can't decide which non-terminal to reduce to without context, and 1-token lookahead is insufficient.
- **Resolution B (lexer-context flag via mid-rule actions) partly works.** Factoring an `if_cond` non-terminal eliminates conflicts in the IF case. But applying the same pattern to FOR creates "rule never reduced" warnings and breaks for-while / for-c-style parsing. The shared FOR prefix across 5 alternatives (bare, while, c-style, range-1, range-2) doesn't tolerate mid-rule action factoring cleanly.
- **Empirical conclusion: this grammar needs a more invasive redesign**, not a localized fix. Options that actually work:
  - **GLR parser** (`%glr-parser` Bison directive). Tolerates ambiguity at parse time, disambiguates via `%dprec` or merge actions. Pays a runtime cost but solves the fundamental issue.
  - **Lexer-level context tracking that doesn't depend on parser mid-rules.** Track "we just saw IF/FOR/SWITCH/SELECT, the next LBRACE at depth-0 is a block" directly in the lexer's state, independent of Bison's reduction order. Requires parser-lexer cooperation but doesn't fight Bison's LR limits.
  - **Different syntax** for composite literals: `&Point{...}` or `.{...}` or `[Point{x:1, y:2}]` — picks a grammar that's unambiguous by construction. Diverges from Go aesthetic.

The recommendation is to **stop the focused grammar-landing attempts** until one of these paths is committed. The current session's M10-composite-literal-spike + grammar attempt has surfaced enough empirical signal to choose direction.

## What was tried this session

### Attempt 1: Parallel `expression_nc` hierarchy (Resolution A)

Added `expression_nc / unary_expr_nc / postfix_expr_nc / binary_expr_nc / primary_expr_nc` parallel cascade, with `primary_expr_nc` omitting `struct_lit`. Used in IF and FOR cond positions.

```
bison -d -o src/parser/parser.tab.c src/parser/parser.y
src/parser/parser.y: conflicts: 65 shift/reduce, 386 reduce/reduce
```

**386 reduce/reduce.** The minimal-grammar test passed because the minimal grammar didn't have the recursive cascade through binary_expr and operator productions. In the real grammar, every operator (`+ - * / < > == && ||`) recurses through `expression`, and the parallel `expression_nc` shares all the leaves. Bison can't decide which non-terminal to reduce a bare IDENT to.

This rules out the audit's recommended resolution.

### Attempt 2: Lexer-context flag via mid-rule actions (Resolution B)

Approach:
- New `LBRACE_BODY` Bison token, emitted by the lexer bridge instead of `LBRACE` when `g_no_clit_depth > 0` AND `g_paren_depth == 0`.
- `block` rule accepts both `LBRACE stmts RBRACE` and `LBRACE_BODY stmts RBRACE`.
- `struct_lit` accepts only `LBRACE`.
- Parser mid-rule actions in if_stmt / for_stmt bracket the cond expression: `IF { ++flag; } expression { --flag; } block`.

#### IF case: works ✓

Factoring the IF cond into a sub-rule (`if_cond: IF { ++flag; } expression { --flag; $$ = $3; }`) eliminates the per-alternative epsilon-naming conflict. With this, `if X { body }` correctly emits LBRACE_BODY and the parser reduces IDENT → expression → if_cond → block. Bison reports 0 added reduce/reduce conflicts and the IF works as designed.

#### FOR case: doesn't factor cleanly ✗

The FOR has 5 alternatives:
```
for_stmt:
    FOR block
    | FOR expression block             /* while-style */
    | FOR simple_stmt SEMI expression SEMI simple_stmt block  /* c-style */
    | FOR identifier SHORT_ASSIGN RANGE expression block       /* range 1-var */
    | FOR identifier COMMA identifier SHORT_ASSIGN RANGE expression block  /* range 2-var */
```

All share the FOR prefix. Adding a mid-rule action `FOR { ++flag; }` to the while-style and c-style alternatives:
- Direct addition (no factoring): "rule never reduced because of conflicts" warning. The @N epsilons in while and c-style alternatives compete with each other and with bare `FOR block`.
- Factoring `for_prefix: FOR { ++flag; };` shared by while and c-style: Bison commits too early. After `FOR IDENT lookahead`, the parser has to choose between `for_prefix simple_stmt ...` (c-style with init starting with IDENT) and `for_prefix expression block` (while with cond starting with IDENT) and `FOR identifier SHORT_ASSIGN RANGE...` (range). Different mid-rule reductions create reduce/reduce ambiguity.

Empirically: with for_prefix, baseline_probe.goo's c-style for at line 87 fails to parse. Reverting to no-mid-rule on FOR fixes c-style but leaves for-while with the original IDENT . LBRACE ambiguity.

A working subset: factor only `if_cond` (IF case works), leave FOR alone (for-while cond mis-parses with bare identifier). This is a *partial* fix that handles 80% of cond positions in real code.

#### Cross-rule state leakage

Even with just the if_cond factoring (no FOR changes), tests failed in surprising places. `examples/baseline_probe.goo` got a parse error at line 104 (`if total == 6 { ... }`) — but the same `if total == 6 { ... }` pattern worked in isolation. The for-range body at lines 101–103 somehow corrupted parser state before the if.

Further isolation (`/tmp/test_range_simple.goo`) reproduced: a for-range body containing `fmt.Println(total)` failed to parse the `.Println` selector. The struct_lit addition + if_cond mid-rule combined to break selector-access parsing inside for-range bodies.

The reduce/reduce conflict count was still 156 (baseline) but the LBRACE_BODY token's interaction with the existing 156 reduce/reduce conflicts (which Bison resolves silently with default-reduce-earliest) shifted some Bison decisions in subtle ways. Hard to predict without exhaustive state-table inspection.

### Empirical conclusion

**The current Goolang grammar carries ~217 baseline conflicts (61 shift/reduce + 156 reduce/reduce)** that Bison resolves silently via defaults. Any addition that interacts with the same states (and `struct_lit` does — it touches `primary_expr` which is the deepest hot path) will perturb those defaults in unpredictable ways.

The parallel-hierarchy approach (A) is fundamentally incompatible with shared-leaf grammars. The mid-rule-action approach (B) works in clean isolation but interacts unpredictably with the existing conflict mass.

## Path forward (recommendation)

### Option 1: GLR parser

Add `%glr-parser` to parser.y. Bison's GLR mode handles ambiguity by trying multiple parses in parallel, then disambiguating via `%dprec` priorities or merge functions. Conflicts become "this is OK, try both" rather than "Bison can't decide."

Pros:
- Solves the underlying issue at the parser-generator level.
- Existing 217 conflicts effectively become tolerable.
- struct_lit + cond disambiguation expresses cleanly as `%dprec` priorities.

Cons:
- Runtime cost (typically 2–10x slower parsing in ambiguous regions).
- Different debugging model — GLR conflicts surface as runtime ambiguity rather than parse-table conflicts.
- Wholesale change to parser model; risky in a working compiler.

### Option 2: Pre-parser tokenization layer

Move the LBRACE/LBRACE_BODY disambiguation entirely into the lexer (not into mid-rule actions). The lexer maintains a separate context stack:
- When it emits IF/FOR/SWITCH/SELECT, push a "may-need-LBRACE_BODY" frame.
- Track paren/bracket depth as a separate counter.
- When it emits LBRACE: if the top frame is "may-need-LBRACE_BODY" AND depth==0, emit LBRACE_BODY and pop the frame.

This avoids the Bison-mid-rule entanglement entirely. The lexer state machine knows "we just saw IF and we're at depth 0" without needing parser cooperation. The parser sees a grammar with just LBRACE/LBRACE_BODY tokens and no mid-rule actions.

Pros:
- No Bison-level conflicts beyond the inherent 217.
- Lexer state is simpler than parser state for this purpose.
- Composes well with future similar cases (switch/select).

Cons:
- Lexer becomes context-sensitive, complicating future grammar evolution.
- Need to handle edge cases (parenthesized conds, nested ifs in expressions, lambda bodies).

### Option 3: Defer composite literals to a syntax-design pass

Accept that Goolang's grammar isn't ready for Go-style composite literals. Use a different surface syntax that's unambiguous:
- `Point.{x: 3, y: 4}` (Zig-style)
- `&Point{x: 3, y: 4}` (always-pointer)
- `Point[x: 3, y: 4]` (square brackets, reusing existing `[...]` machinery)

Pros:
- No grammar work needed for the cond disambiguation.
- Could enable richer literal forms.

Cons:
- Diverges from Go aesthetic which the project has otherwise honored.
- Existing Goolang programmers (if any) would face a syntax migration.

## What survives from this session

- `AST_STRUCT_LITERAL` enum and `StructLiteralNode` declaration in `include/ast.h` and `src/ast/ast.c`. The new AST kind is at the END of the enum (so it doesn't shift other values — an earlier mistake made baseline_probe fail). Useful scaffolding for whichever resolution we eventually adopt.
- `M10-composite-literal-spike` doc (the first one) and this follow-up doc.
- Empirical Bison conflict counts: 217 baseline. Useful as a regression check.

The current `parser.y` and `lexer_bridge.c` are reverted to their pre-session state. `make verify` is 5/5 green.

## Open questions

- **Q1.** Which of the three options does the operator want to commit to? GLR is the most general but most disruptive. Lexer-layer is more surgical but adds context-sensitivity. Defer-to-syntax-redesign is the smallest immediate change but a larger downstream commitment.
- **Q2.** What's the actual user-facing volume of "bare struct literal in cond" code? If it's rare, the parens-required workaround (Go's de-facto behavior) may suffice via documentation rather than a parser fix.
- **Q3.** Are the 156 baseline reduce/reduce conflicts known to anyone, or are they accumulated tech debt? Documenting them (or auditing them in a separate task) would help future grammar work.

## Conclusion

The M10-composite-literal-grammar task as scoped — a single Bison-grammar landing — is not achievable with the current parser architecture. The actual unlock requires either a parser-generator change (GLR), a lexer-architecture change (context-tracking lexer), or a language-design change (different surface syntax). Each is a substantial commitment that wants its own scoping pass.

Releasing M10-composite-literal-grammar as cancelled with this lesson. Filing M10-grammar-strategy as a new task to choose between the three options before any further code lands.
