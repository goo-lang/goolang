# Bison conflict ledger

Baseline: **82 shift/reduce + 256 reduce/reduce** — verified 2026-07-04 on
`feat/type-assertions` (Task 3, type switch). Previous baseline 81/256 verified
2026-07-04 on main @ 32a53d1. The tripwire (`scripts/grammar-tripwire.sh`) asserts
these numbers exactly; treat any delta as a stop-the-line event, not noise.

## Baseline history (every change was justified in its PR, never absorbed silently)

| When | Change | Why it was accepted |
|---|---|---|
| pre-#92 | 78 S/R + 256 R/R | historical baseline |
| #92 (boolean NOT) | 78 → **79** S/R (+1) | BANG joined the pre-existing 8-token shift-wins family in the func_signature→func_result states; differentially verified zero parse-behavior change |
| #106/#107 (closures) | 79 → **81** S/R (+2) | func-type/literal productions; justified with state inspection + differential run, recorded as the new baseline |
| #108 (ARROW precedence) | **zero delta** | precedence move produced a byte-identical conflict distribution — the bar a "risky" grammar change can meet |
| #109 (struct embedding) | zero delta | new productions all semicolon-terminated |
| #111 (stdlib unblockers) | zero delta | three grammar-touching tasks (trailing commas, slice-conversion arm, spread arm), 0 new conflicts |
| type-assertions Task 3 (type switch) | 81 → **82** S/R (+1) | new `SWITCH type_switch_guard LBRACE type_case_list RBRACE` plain-LBRACE fallback arm (mirroring the pre-existing plain-switch fallback, `SWITCH expression LBRACE case_clause_list RBRACE`) joins the SAME struct-lit-vs-switch-fallback ambiguity family that arm already has in baseline — `identifier` after `SWITCH` can continue into `struct_lit`'s `identifier LBRACE …` on the same plain LBRACE token the fallback arm itself wants. Dead in practice: the M10 lexer-bridge cond-frame (lexer_bridge.c) always emits `LBRACE_BODY` — never plain `LBRACE` — for a real switch body brace at paren_depth 0, regardless of guard/tag complexity, so the fallback arm (and this conflict) is only reachable if the bridge itself has a bug. See "Justification detail" below. |

## What a justified delta looks like (the #92 precedent)

1. `bison -Wcounterexamples` on the new grammar; extract each NEW conflict's counterexample.
2. Classify: does the conflict land in an existing, understood family (e.g. the shift-wins
   family in func-result states), or is it a new ambiguity class?
3. Prove parse behavior unchanged where it matters: run the full golden suite
   (`bash scripts/run_golden.sh`, must stay N/0) AND write targeted probes for the exact
   token sequences the counterexamples describe, comparing against expected parses.
4. Only then: update `EXPECTED_SR`/`EXPECTED_RR` in `scripts/grammar-tripwire.sh` and add a
   row to the table above, IN THE SAME COMMIT as the grammar change, with the justification.

A delta without steps 1–3 is a revert, not a negotiation.

## Differential verification recipe

For grammar changes that should be behavior-neutral (precedence moves, refactored arms):
build both compilers (before/after), run the golden corpus through each, and byte-compare
outputs. #108 additionally compared the conflict *distribution* (bison's per-state report),
not just the totals — totals can stay equal while conflicts migrate.

## Justification detail: type-assertions Task 3 (81 → 82 S/R)

Ran the full procedure (steps 1–3 above) before touching `EXPECTED_SR`:

1. **Counterexamples**: `bison -Wcounterexamples` before/after (`git show HEAD:src/parser/parser.y`
   built separately) isolated the delta to three new `shift/reduce conflict on token LBRACE`
   sites, all with the shape `SWITCH type_switch_guard … primary_expr DOT LPAREN TYPE RPAREN …
   struct_lit … identifier • LBRACE …` (shift) vs `SWITCH expression … primary_expr …
   identifier •` (reduce, the plain switch's OWN fallback arm claiming the bare identifier as
   its complete tag). Net total delta is only +1 (not +3): bison's per-state analysis shows
   most of this family's states were already conflicted in baseline via the plain-switch
   fallback arm alone: this task's new fallback arm reaches those SAME states via a second path
   (state merging), except for exactly one genuinely new state.
2. **Classify**: same family as the plain switch's existing (already-baselined) fallback-arm
   ambiguity — `identifier` after `SWITCH` can either complete as a bare tag/guard expression
   (with the switch body's `{` immediately following) or continue into a struct literal
   (`identifier LBRACE …`), and on the single plain-`LBRACE` token both readings are live. Not a
   new ambiguity *class* — the type-switch fallback arm was requested by this task's own brief
   specifically to mirror the plain switch's existing fallback (parser.y's `switch_stmt` already
   has this exact pattern for the tagged AND tagless forms).
3. **Prove behavior unchanged**:
   - Full golden suite: 230 passed, 0 failed (228/0 baseline + this task's 2 new goldens).
   - `make test`: 76/1 (unchanged).
   - The fallback arm is unreachable for any well-formed program: the M10 lexer-bridge cond-frame
     (`lexer_bridge.c`, `m10_push_frame`/pop) pushes a SWITCH frame and pops it — emitting
     `LBRACE_BODY`, not `LBRACE` — at the FIRST `{` seen at the frame's own paren_depth,
     regardless of how long or complex the preceding guard/tag expression is. A real type-switch
     body brace is therefore ALWAYS `LBRACE_BODY`, never `LBRACE`, so the ambiguous fallback
     state is never entered in practice — exactly like the plain switch's identical, already-
     accepted fallback-arm conflict.
   - Targeted differential probes (go run vs `bin/goo`, byte-identical): struct literal as a
     `case` bound value (`v.Field`), a method call on the bound value (`v.Method()` — this
     surfaced an UNRELATED real bug, since fixed: codegen must mirror `v` into the type
     checker's OWN scope, not just codegen's value table, because codegen re-invokes
     `type_check_call_expr` internally for method-call resolution), `case nil:`, `break` inside
     a case, and a pointer-concrete case type (`case *Box:`) composing with #113's boxing.

Conclusion: justified delta, not a revert. `EXPECTED_SR` updated 81 → 82 in
`scripts/grammar-tripwire.sh` in the same commit as the grammar change.
