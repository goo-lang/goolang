# M10 grammar strategy decision (2026-05-15)

Synthesis of the three empirical probes commissioned by `M10-grammar-strategy`. Decides which approach Goolang will use to resolve the `IDENT . LBRACE` shift/reduce conflict introduced by Go-style struct literals.

## Decision

**Strategy 2: lexer-layer context tracking** is chosen.

Implementation will follow the design in `docs/M10_LEXER_LAYER_PROBE.md`. Estimated cost: ~95–145 LOC across `src/parser/lexer_bridge.c` (bulk of the work, frame stack + push/pop logic) and `src/parser/parser.y` (one new token `LBRACE_BODY` + new alternatives in `block`, `match_expr`, `select_stmt`, `catch_expr`). The `src/lexer/lexer.c` scanner stays stateless.

## Evidence summary

| Strategy | Cost | Empirical verdict | Reference |
|---|---|---|---|
| 1 — GLR parser | 1–3 weeks productionalize + perf cost | **REJECTED.** Bison-builds cleanly with `%glr-parser` (+3 S/R only) but every non-trivial program fails at runtime with "syntax is ambiguous". The 156 baseline R/R conflicts that LALR silently resolves become true runtime ambiguities. Needs 40–80 `%dprec` annotations across the operator hierarchy. | `docs/M10_GLR_PROBE.md` |
| 2 — Lexer-layer | ~95–145 LOC, one tricky edge case | **CHOSEN.** Adds one new token (`LBRACE_BODY`); doesn't perturb the 217 baseline conflicts. Scariest edge case (`guard_condition` leaving an IF frame unbalanced because it terminates with COLON not LBRACE) has a known fix: pop top IF_COND frame on depth-matched COLON. | `docs/M10_LEXER_LAYER_PROBE.md` |
| 3 — Alt-syntax | ~5 LOC (D) to ~ same as Go (B) | **REJECTED (with caveats).** Candidate A (`Point.{}`) and Candidate C (`Point[]`) had low conflict counts but were *lethal* at runtime — A broke `fmt.Println` (selectors), C broke `arr[0]` (indexing). B (`&Point{}`) works but forces pointer-by-default for slices. D (`#Point{}`) is grammatically cleanest but diverges from Go aesthetic, which the project has explicitly preserved. | `docs/M10_ALT_SYNTAX_PROBE.md` |

## Rationale for lexer-layer over the others

### Why not GLR

The GLR probe's empirical finding is decisive: even bare `func main() {}` fails to parse under `%glr-parser`. The 156 baseline R/R conflicts (per the baseline conflict audit, `docs/M10_BASELINE_CONFLICTS_AUDIT.md`) become true runtime ambiguities. Resolving them requires ~40–80 `%dprec` priorities plus `%merge` functions, plus a Bison version upgrade (the system Bison 2.3 has a known GLR-mode header-duplication bug). Estimated 1–3 weeks of focused work just to get back to where LALR is today, with permanent runtime overhead on every arithmetic expression.

This was the single biggest surprise in the synthesis: GLR was the *most plausibly viable* strategy on paper (broadest generality, handles ambiguity by design) but is the *least viable* in this codebase because of the LALR-default-resolved conflict mass that GLR exposes.

### Why not alt-syntax

Candidates A and C cannot ship — empirical breakage of selectors and indexing respectively. That leaves B (`&Point{}`) and D (`#Point{}`):

- **B (`&Point{}`) carries a semantic cost.** Forcing every struct literal to be address-of means slices of structs become slices of pointers (`[]User{...}` → `[]*User{...}`). Heap allocation per element rather than contiguous storage. This contradicts both Go semantics and the M7 stdlib-expansion's deliberate Go-source-compatibility shortcut.
- **D (`#Point{}`) breaks the Go-aesthetic story** Goolang has been carrying since the project's start. The seed.json comment and every prior milestone (M7-stdlib-expansion, M8 Foundation Verification, the for-range syntax, the `:=` form) honor Go's surface. Changing struct literal syntax — one of the most common constructs — for a parser convenience would be a meaningful regression in language consistency.

The lesson from this probe also matters: **Bison conflict counts are not enough.** Candidates A and C looked attractive (low conflict deltas) but had silent runtime breakage that only surfaced via `bin/goo`. Any future grammar work must include runtime verification, not just bison-counts.

### Why lexer-layer wins

Three concrete advantages:

1. **Preserves Go syntax exactly.** Users write `Point{x: 3, y: 4}` in non-cond positions and `if (Point{x: 3}.y > 0)` in cond positions — the same idiom Go itself uses. No semantic divergence.

2. **Orthogonal to baseline conflicts.** Adds one new token and one new `block` alternative. Doesn't touch the operator cascade where the 152 unary-vs-binary R/R conflicts live. The previous session's mid-rule-action attempt failed because it perturbed those existing states; the pure-lexer approach avoids this entirely.

3. **Bounded scope with known edge cases.** The probe enumerated the edge cases (`guard_condition`, nested `catch_expr` body, anonymous functions). Each has a documented fix. ~100 LOC is well within a milestone-shaped scope.

The chief risk — context-stack desync if the parser hits an error mid-cond — is mitigated by resetting the stack in `parse_input` (already a pattern in the previous session's lexer-bridge changes for `g_paren_depth`).

## What the baseline conflict audit told us (in hindsight)

The audit was load-bearing in an unexpected way. It documented:
- 152 of 156 R/R conflicts come from the unary-vs-binary operator cluster.
- Bison's default-reduce resolution is *correct* in every case.

Two strategy implications followed from this:

- **For GLR:** those 152 conflicts become 152 *runtime ambiguities* that must each be explicitly resolved. The audit's "don't bother cleaning up" recommendation (which is right for LALR) becomes the OPPOSITE for GLR — cleanup IS required. This made GLR much more expensive than it appeared.
- **For lexer-layer:** the 152 conflicts are in operator-cascade states that the LBRACE/LBRACE_BODY tokenization doesn't touch. So the baseline is structurally safe to coexist with. Confirmed empirically: the lexer-layer probe expects zero added conflicts.

The audit didn't predict GLR's failure mode, but the data it gathered was exactly what made the GLR probe interpretable when it failed.

## Next steps

1. **Cancel `M10-composite-literal-grammar` (already cancelled in prior session).** Re-cancellation not needed.
2. **File `M10-lexer-layer-impl`** as the concrete implementation task, blocked on this decision doc. Scope: implement the state machine designed in `M10_LEXER_LAYER_PROBE.md`. Acceptance: `bin/goo` parses `Point{x:3, y:4}` outside conds, rejects `if Point{x:3}.y > 0 {body}` (suggests parens), passes existing `make verify` 5-gate net.
3. **File `M10-struct-literal-impl`** as the type-check + codegen task, blocked on `M10-lexer-layer-impl`. Scope: AST handling for the `StructLiteralNode` already scaffolded in `include/ast.h` from the prior session, plus codegen. Acceptance: `p := Point{x:3, y:4}; fmt.Println(p.x)` prints `3` end-to-end.
4. **File `M10-probe-gate`** as the regression-net entry. Scope: `examples/m10_probe.goo` + `make m10-probe` joining `make verify` once the impl lands. Same pattern as the comptime-probe promotion.

The original M10 decomposition (struct literal + collection literal + range two-var + variadic println + probe gate) is mostly already shipped or cancelled. The above two impl tasks replace the cancelled `M10-composite-literal-grammar` with the lexer-layer-grounded approach. Map literals (`map[K]V{...}`) already work in the existing grammar; slice literals in expression position (`[]int{1,2,3}`) need similar but separate work, deferrable.

## Lessons learned (for future grammar audits)

1. **Run probes on the real grammar, not minimal repros.** The original M10 spike's parallel-hierarchy recommendation passed a minimal-grammar test and failed in production because the minimal grammar didn't have the recursive operator cascade.
2. **Verify with `bin/goo`, not just `bison -d`.** Bison's default-shift resolution can produce a "successful" parse that silently breaks unrelated language features (per alt-syntax A and C).
3. **GLR is not a free pass to ignore LALR conflicts.** Existing conflicts that LALR resolves silently become runtime errors under GLR. Cleanup-before-GLR is required, not optional.
4. **Lexer-layer disambiguation is the cheapest way to add Go-style context-sensitive parsing on top of an LALR grammar** — significantly cheaper than GLR migration or grammar refactoring.

## Conclusion

Implementation proceeds with **strategy 2 (lexer-layer)**. The probe docs and this decision doc together give the next implementer enough information to write the impl task without further audit work.
