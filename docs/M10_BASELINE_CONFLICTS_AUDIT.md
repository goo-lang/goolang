# M10 baseline conflict audit (2026-05-15)

Categorizes the 217 baseline Bison conflicts (61 shift/reduce + 156 reduce/reduce) in `src/parser/parser.y`. Output: a recommendation about whether to clean up before pursuing M10 grammar work or leave them alone.

## TL;DR

- **Don't clean up.** 152 of the 156 reduce/reduce conflicts (97%) come from a single well-understood pattern: unary-vs-binary operator ambiguity (`+`, `-`, `*`, `&`). Bison's default resolution (prefer earlier rule) is *correct* in every case and has been stable across the project's history.
- **The 4 high-density states are duplicate-shape**, not 4 independent problems. They differ only by which unary/binary operator is at the conflict point.
- **Implications for M10 grammar work:**
  - GLR strategy: dissolves these conflicts at runtime cost. Best safety margin.
  - Lexer-layer strategy: orthogonal — adds a new token and a `block` alternative without touching any of the high-traffic conflict states. Lowest perturbation.
  - Alt-syntax strategy: uses leading tokens that aren't IDENT, so doesn't add to the IDENT . LBRACE conflict at all.
- **Cleanup-first is NOT recommended.** Removing the baseline conflicts would require restructuring the unary expression hierarchy and the return-stmt grammar. Both are stable, working, well-resolved. The cleanup would itself be a multi-week refactor with no functional benefit.

## Conflict landscape

```
$ bison -d --verbose -o src/parser/parser.tab.c src/parser/parser.y 2>&1 | tail -2
src/parser/parser.y: conflicts: 61 shift/reduce, 156 reduce/reduce
```

State-level breakdown from `parser.output`:

| Cluster | States | Conflicts | Pattern |
|---|---|---|---|
| A — Unary/binary operator ambiguity | 263, 265, 267, 281 | 4×38 = **152 reduce/reduce** | `expression: unary_expr .` vs `unary_expr: OP unary_expr .` |
| B — return_stmt optional value | 317 | 19 shift/reduce | `RETURN .` vs `RETURN . expression` |
| C — call_expr vs primary_expr | 251 | 8 shift/reduce | After `primary_expr`, lookahead `LPAREN` |
| D — index_expr in expr | 254/356 | 8 shift/reduce | After `primary_expr`, lookahead `LBRACKET` |
| E — postfix increment/decrement | 393 | 4 shift/reduce | After `primary_expr`, lookahead INCREMENT/DECREMENT |
| F — Miscellaneous | many small | ~30 remaining | various |

The four states in cluster A are structurally identical — same rules conflict, just different operator tokens (PLUS, MINUS, MULTIPLY, BIT_AND). So it's "**one bug, four copies**" rather than four distinct problems.

## Cluster A: detail

State 263 (representative):

```
state 263

  128 expression: unary_expr .
  138 unary_expr: PLUS unary_expr .

    $end          reduce using rule 128 (expression)
    $end          [reduce using rule 138 (unary_expr)]
    CONST         reduce using rule 128 (expression)
    CONST         [reduce using rule 138 (unary_expr)]
    [... 36 more terminals, same pattern ...]
```

What's happening: after parsing `+X`, Bison has both `expression: unary_expr .` (treat `+X` as a complete expression) and `unary_expr: PLUS unary_expr .` (treat it as a unary expression waiting to be wrapped) available. Bison resolves to rule 128 (expression) for every lookahead, suppressing rule 138.

Why this works correctly: Goolang's grammar treats `unary_expr` as a *kind of* `expression`. When the parser sees `+X` and then any non-operator token, the right answer is "this is an expression"; the suppression is correct.

Could this be cleaned up? Yes, by inverting the cascade — making `expression` flow up from `unary_expr` rather than including it as a sibling. But that's a wholesale restructuring of `expression / unary_expr / postfix_expr / binary_expr` for no functional gain.

## Cluster B: detail

State 317:

```
state 317

  116 return_stmt: RETURN .
  117            | RETURN . expression
  118            | RETURN . expression COMMA expression_list

    IDENTIFIER      shift, and go to state 4
    STRING_LITERAL  shift, and go to state 73
    [... 17 more shift actions, all into expression-starting states ...]
```

What's happening: after `RETURN`, Bison can either reduce immediately (no-value return) or shift to parse an expression. Default-shift wins, attempting to parse an expression.

Why this is correct: in Goolang/Go syntax, `return` followed by anything that could start an expression IS the value-returning form. The bare `return` only appears followed by `}` (block end) or `;` (statement separator). Default-shift produces the right parse.

This conflict is essentially a function of Goolang/Go's grammar not separating "return" from "return X" syntactically — they share a prefix and use lookahead to disambiguate. Same pattern in Go's reference parser.

## Cluster C/D/E: short notes

- **C (call_expr)**: After `primary_expr` with lookahead `LPAREN`, Bison can either reduce (treating primary as a complete expression) or shift (extending to call_expr). Default-shift correctly extends.
- **D (index_expr)**: Same pattern with `LBRACKET`.
- **E (postfix)**: Same with `INCREMENT/DECREMENT`.

All three are "should I extend this expression or stop here?" conflicts with default-shift being correct.

## Cluster F: miscellaneous

About 30 conflicts scattered across smaller states. Spot-checked a few:

- Useless productions for GPU constructs (`KERNEL`, `DEVICE`, etc.) flagged by Bison as "useless nonterminals" — these were disabled per the comment block at `parser.y:1075–1083`. They don't actually parse; the conflict reports for them are vestigial.
- `try_expr` / `catch_expr` / `match_expr` interactions with `expression` — Goo extensions that compete for the same expression slot. Default resolution is fine.
- WebAssembly export/import statements (`export STRING_LITERAL`, etc.) — similar to GPU constructs, partially-implemented features.

None of cluster F appears to be systemically problematic.

## What this means for M10 grammar strategies

| Strategy | Interaction with baseline conflicts | Risk |
|---|---|---|
| **GLR** | Dissolves all conflicts at runtime; baseline becomes irrelevant | LOW (in terms of conflict-interaction). Other risks: runtime overhead, debugging model change |
| **Lexer-layer** | Adds 1 new token (`LBRACE_BODY`), 1 new alternative in `block` rule. Doesn't touch primary_expr or any high-conflict state. | LOW. Closest to "just add features without perturbing existing resolution" |
| **Alt-syntax** | Uses leading tokens that aren't IDENT (`.`, `&`, `#`, etc.) so the IDENT . LBRACE state is never created | LOW. But shifts the syntax-design burden onto the user |

The previous session's attempt at Resolution B with mid-rule actions in IF/FOR was *higher* risk because mid-rule actions create new epsilon productions that compete with existing rules in the same states. The pure lexer-layer approach (where the lexer makes the decision without parser cooperation) avoids this entirely.

## Recommendation

**Do not pursue baseline conflict cleanup as a prerequisite to M10 grammar work.** The cleanup would be:
- 1–2 weeks of grammar refactoring
- Zero functional gain
- Risk of destabilizing the working parser

Instead, choose a grammar strategy (GLR / lexer-layer / alt-syntax) that *doesn't depend* on the baseline being clean. All three options either ignore the baseline or are orthogonal to it.

## Open questions

- **Q1.** Is anyone tracking these conflicts as tech debt? The "useless rule" warnings for GPU constructs and `try_expr`/`catch_expr` interactions suggest there's accumulated cleanup that's deliberately deferred. Document this somewhere durable so future grammar work doesn't trip on the pattern again.
- **Q2.** Should `bison -d` warnings about "useless nonterminals" be gated in CI? Right now they're noise; if we expect zero, we'd catch any new useless productions. Cheap signal.

## Conclusion

The 217 baseline conflicts are mostly **152 copies of one well-understood pattern** plus a handful of "extend-or-stop" decisions with correct default resolution. They are NOT a barrier to M10 grammar work. Pick a strategy that's orthogonal to them.
