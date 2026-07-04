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
| type-assertions Task 3 (type switch) | 81 → **82** S/R (+1) | **Corrected 2026-07-04** (the original row below was factually wrong — see "Corrected justification"). The new `type_switch_guard` nonterminal is reachable from switch_stmt's `SWITCH type_switch_guard LBRACE_BODY type_case_list RBRACE` arm — the LIVE arm every real type switch takes, NOT the plain-LBRACE fallback. Reaching `type_switch_guard` means `primary_expr`'s existing `struct_lit: identifier LBRACE …` alternative is now reachable from a new position (right after `SWITCH identifier`), which collides with the plain switch's OWN pre-existing fallback arm (`SWITCH expression LBRACE case_clause_list RBRACE`, already in baseline before Task 3) reducing the same bare `identifier` as a complete tag on the same LBRACE lookahead. Verified empirically NOT isolated to the plain-LBRACE type-switch fallback arm: removing EITHER type-switch arm alone (keeping the other) leaves the count at 82; only removing `type_switch_guard`'s reachability entirely (both arms) restores 81. Separately, the specific ambiguous token sequence (`identifier` directly followed by plain `LBRACE` in tag/guard position) is proven lexically unreachable for either switch form — the M10 bridge unconditionally converts the first depth-0 `{` after SWITCH to LBRACE_BODY (lexer_bridge.c:235-246), so a bare unparenthesized struct literal as a switch tag/guard operand is a parse error (verified with compiled probes), not a silent misparse; parenthesizing it works. See "Corrected justification" below. |

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

## Corrected justification: type-assertions Task 3 (81 → 82 S/R)

**This section replaces an earlier version of this entry that was factually wrong.** The
original write-up (see git history of this file) claimed the +1 conflict was "isolated to
the plain-LBRACE fallback arm" (`SWITCH type_switch_guard LBRACE type_case_list RBRACE`) and
"dead in practice" because that fallback arm is supposedly unreachable. Both claims were
false: the conflict is fully reproducible through the LIVE `LBRACE_BODY` arm alone (the one
every real type switch uses), with the dead fallback arm removed entirely. Re-derived from
scratch below with `bison -Wcounterexamples` and compiled differential probes.

1. **Counterexample** (`bison -Wcounterexamples -d -o /tmp/pcx.tab.c src/parser/parser.y`,
   full run, no `git show`/separate-build shortcut): the delta isolates to three
   `shift/reduce conflict on token LBRACE` sites (not `LBRACE_BODY`), each with this shape:

   ```
   First example (shift): ... FUNC identifier LBRACE SWITCH identifier • LBRACE struct_lit_inits RBRACE
       DOT LPAREN TYPE RPAREN LBRACE_BODY type_case_list RBRACE RBRACE block $end
     switch_stmt: SWITCH type_switch_guard • LBRACE_BODY type_case_list RBRACE
       type_switch_guard: primary_expr • DOT LPAREN TYPE RPAREN
         primary_expr: struct_lit
           struct_lit: identifier • LBRACE struct_lit_inits RBRACE
   Second example (reduce): ... FUNC identifier func_signature LBRACE SWITCH identifier • LBRACE
       case_clause_list RBRACE RBRACE $end
     switch_stmt: SWITCH expression • LBRACE case_clause_list RBRACE
       expression: unary_expr: primary_expr: identifier •
   ```

   The **shift** side reaches all the way through rule `SWITCH type_switch_guard LBRACE_BODY
   type_case_list RBRACE` (parser.y:1413, the arm every compiled type switch actually takes) —
   it is NOT the plain-LBRACE fallback arm (parser.y:1417). The conflict token itself (plain
   `LBRACE`) is the struct literal's OWN opening brace, consumed deep inside `primary_expr` via
   `struct_lit: identifier LBRACE …`, long before the guard's `DOT LPAREN TYPE RPAREN` tail (and
   therefore long before LBRACE_BODY-vs-LBRACE arm selection) is even reached. The **reduce**
   side is `SWITCH expression LBRACE case_clause_list RBRACE` (parser.y:1382) — the ordinary
   switch's OWN pre-existing plain-LBRACE fallback, already in baseline before this task and
   unrelated to type switch. So the real fork is: after `SWITCH identifier`, on lookahead
   LBRACE, keep building a struct literal that could become the type-switch guard's operand
   (shift) vs. treat the bare identifier as a complete ordinary-switch tag (reduce, the
   pre-existing arm). Net delta is +1, not +3: two of the three sites (struct_lit's
   trailing-comma and empty-literal completions) reuse LALR states already conflicted in
   baseline via the plain switch's own struct-lit-vs-tag ambiguity (a struct literal can
   already be an ordinary switch's whole tag, e.g. `switch Point{X:1} { … }`); only one
   genuinely new state is added by `type_switch_guard` becoming reachable.
2. **Classify**: same ambiguity *family* as the plain switch's existing (already-baselined)
   struct-lit-vs-bare-tag conflict — not a new class. `type_switch_guard`'s operand is an
   ordinary `primary_expr` (parser.y:1434), which already includes `struct_lit` as one of many
   alternatives; giving `switch_stmt` a new arm that reaches `primary_expr` from a position
   right after `SWITCH` necessarily re-exposes this pre-existing ambiguity, regardless of
   which trailing arm (LBRACE_BODY or plain-LBRACE) eventually consumes the completed guard.
   **Empirically verified** (three isolated `bison -d` builds, scalar counts only — cheap,
   no `-Wcounterexamples`): removing only the plain-LBRACE fallback arm (1417) → still 82;
   removing only the LBRACE_BODY arm (1413) → still 82; removing both → 81. Each arm is
   independently sufficient to reproduce the conflict, which refutes "isolated to the
   [dead] fallback arm" outright.
3. **Prove the shift-resolution is behavior-correct** (compiled differential probes,
   `bin/goo`, real exit codes):
   - Full golden suite: 230 passed, 0 failed. `make test`: 76 passed / 1 skipped. Both
     unchanged by this ledger-only fix (no grammar edits made).
   - Ordinary type switches (bind-less and bound forms) and an ordinary value assertion used
     directly as a plain switch's tag (`switch x.(int) { case 42: … }`, exercising
     `primary_expr DOT LPAREN type RPAREN` — Task 1's selector_expr — as `expression`) all
     compile and run correctly, confirming the TYPE-vs-`type` fork the parser.y:1396-1412
     comment describes is genuinely conflict-free (no counterexample ever names the `DOT
     LPAREN TYPE RPAREN` tail; every one of the three sites above is resolved before that
     point is reached).
   - **The specific ambiguous token sequence is lexically unreachable, symmetrically, for
     both switch forms** — this is the corrected replacement for the old "dead in practice"
     claim, now with compiled evidence instead of an assumption:
     - `switch Point{X: 1} { case Point{X: 1}: … }` (unparenthesized struct literal as an
       ordinary switch's tag) → `Parse error: syntax error` (bin/goo, exit 1).
     - `switch Point{X: 1}.(type) { case Point: … }` (unparenthesized struct literal as a
       type-switch guard operand) → `Parse error: syntax error` (bin/goo, exit 1) — same
       failure mode, same cause.
     - `switch (Point{X: 1}) { … }` and `switch (Point{X: 1}).(type) { … }` (parenthesized)
       → both parse cleanly (the type-switch one fails later at type-check with "operand is
       not an interface type", a correct and unrelated semantic rejection — Point isn't
       boxed into an interface here).
     - Root cause (`lexer_bridge.c:235-246`): the M10 bridge converts the FIRST `{` seen at
       the SWITCH cond-frame's OWN push paren_depth into `LBRACE_BODY` unconditionally, and
       pops the frame right there — it does not look at what precedes the brace. An
       unparenthesized struct literal's own opening brace sits at that same depth-0 position,
       so it is mis-tokenized as `LBRACE_BODY` (which `struct_lit` cannot consume) before the
       grammar-table conflict is ever reached, for EITHER switch form. Parens raise
       `paren_depth`, moving the struct literal's brace to depth ≥ 1 so it is emitted as plain
       `LBRACE` and the frame survives to correctly tag the real body brace later. This
       mirrors Go's own static rule requiring parens around composite literals in `if`/`for`/
       `switch` headers — enforced here by the lexer instead of a grammar restriction.
   - Aside (out of scope for this ledger fix, flagged for a future task): a bind-less type
     switch dispatching on primitive types passed as bare literals into an `interface{}`
     parameter (e.g. `describe(42)` / `describe("hi")` / `describe(3.14)` against
     `switch x.(type) { case int: … case string: … }`) always matched the first case in
     ad-hoc testing — looks like a pre-existing semantic/codegen dispatch bug independent of
     this grammar conflict (the existing `type_switch_probe` golden avoids it by testing
     struct types through the bound form). Not investigated further — touching
     typecheck/codegen is outside this task's scope.

Conclusion: **inherent, not eliminated.** Left-factoring the `SWITCH … primary_expr DOT
LPAREN` prefix (the fix this ambiguity's own description originally suggested) would not
touch this conflict — it lives entirely inside `struct_lit`, resolved long before `DOT LPAREN`
is reached. The only factoring that would remove it is giving `type_switch_guard` a
struct-lit-free duplicate of the entire `primary_expr` grammar (call_expr/index_expr/
selector_expr all recurse through `primary_expr`, so this is not a local, single-token
factor — it is a wholesale fork of the expression grammar) to eliminate an ambiguity that:
(a) already exists in baseline for the plain switch's own tag position, unmodified by this
task, and (b) is proven lexically unreachable for real programs either way. That fails the
skill's "don't balloon complexity" bar for zero behavior gain. `EXPECTED_SR` stays 82 in
`scripts/grammar-tripwire.sh`; this correction changes only the justification, in the same
commit as the ledger fix.
