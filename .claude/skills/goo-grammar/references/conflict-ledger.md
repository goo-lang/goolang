# Bison conflict ledger

Baseline: **84 shift/reduce + 256 reduce/reduce** — verified 2026-07-08 on
`feat/go-syntax-grammar-a` (gofmt-syntax sub-project A, Task 2, switch-with-init).
Previous baseline 82/256 verified 2026-07-04 on `feat/type-assertions` (Task 3, type
switch). The tripwire (`scripts/grammar-tripwire.sh`) asserts these numbers exactly;
treat any delta as a stop-the-line event, not noise.

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
| gofmt-syntax-a Task 2 (switch-with-init) | 82 → **84** S/R (+2) | `switch_stmt` gained four init arms mirroring IF's init pattern (parser.y:1552-1591): two for plain `SWITCH simple_stmt SEMICOLON expression LBRACE[_BODY] case_clause_list RBRACE`, two for type-switch `SWITCH simple_stmt SEMICOLON type_switch_guard LBRACE[_BODY] type_case_list RBRACE` (reusing `type_switch_guard` unmodified — no new type-switch surface). Both new conflicts are the SAME already-documented families reappearing at new reachable LALR positions, not new ambiguity classes — see "Justification: gofmt-syntax-a Task 2" below. R/R count unchanged (256), confirming no new reduce/reduce ambiguity was introduced. |

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

## Justification: gofmt-syntax-a Task 2 (switch-with-init, 82 → 84 S/R)

Four new `switch_stmt` arms (parser.y:1552-1591) mirror IF's init arm (parser.y:1270)
onto SWITCH: `SWITCH simple_stmt SEMICOLON expression LBRACE[_BODY] case_clause_list
RBRACE` (plain switch) and `SWITCH simple_stmt SEMICOLON type_switch_guard
LBRACE[_BODY] type_case_list RBRACE` (type switch, reusing `type_switch_guard`
unmodified — the bind form `identifier SHORT_ASSIGN primary_expr DOT LPAREN TYPE
RPAREN` already parsed at base without init per Task 3, satisfying spec open point 3's
"scope the init mirror to exactly the base surface, do not add new type-switch
surface"). Both new arms desugar to a wrapping block (`{ init; switch tag {...} }`),
exactly like IF's init arm, so `SwitchStmtNode`/`TypeSwitchNode` needed zero field
changes — no malloc-site audit required.

**Isolated the +2 into two independent +1s** by building with only the plain-switch
arms first (82 → 83), then adding the type-switch arms on top (83 → 84). R/R stayed
256 throughout both steps — no new reduce/reduce ambiguity class.

**Conflict 1 (plain-switch arms, +1, state 539→540 in the isolated build):** NOT a new
state — an EXISTING state gained a second conflict. State 539 (baseline) already held
one conflict (`LBRACE`: shift continues `struct_lit: identifier LBRACE …`, vs. reduce
`primary_expr: identifier` — the Task 3 type-switch-guard-vs-struct-lit ambiguity
above, unchanged). Adding the plain-switch init arm makes `simple_stmt` reachable from
the exact same LALR state (the one dispatching on `identifier` right after `SWITCH`),
merging in `short_var_decl`'s tuple form (`identifier COMMA identifier SHORT_ASSIGN
expression`, rules 69-70) and `simple_stmt`'s tuple-assignment forms. On token
`COMMA`, the SHIFT side carries both `short_var_decl`'s tuple form AND the
identifier-prefixed tuple-assignment arms (rules 138-139); the REDUCE side is
`primary_expr: identifier` feeding the expression-prefixed tuple-assignment arm
(rule 140). This is the SAME shift/reduce ambiguity `simple_stmt` already has
wherever it's reachable — baseline states 439 (statement-position dispatch) and
526 (`FOR identifier •`) hold the identical item-set + COMMA conflict; the
plain-switch arm just makes it ALSO reachable from the switch-tag state, which
happened to already be conflicted for an unrelated reason. Shift wins in all
three positions: `a, s[0] = 1, 2` (reduce-side shape) is a loud parse error at
statement, IF-init, and SWITCH-init positions alike — pre-existing limitation,
unchanged (probed at review). Same "family, new reachable position" shape as
the #92 and Task 3 precedents.
*(Corrected at review 2026-07-08: the original entry cited baseline states
539/581/609/680 as the pre-existing family sites — 581/609/680 are unrelated
conflicts; the real sites are 439 and 526 — and had the shift/reduce sides of
the tuple-assignment split mislabeled. Classification conclusion unchanged;
verified by independent counterexample regeneration.)*

**Conflict 2 (type-switch arms, +1, brand-new state 713 in the full build):** A
genuinely new LALR state, but with the IDENTICAL core items and conflict shape as
baseline's already-documented Task 3 conflict: `type_switch_guard: identifier •
SHORT_ASSIGN …` / `primary_expr: identifier •` / `struct_lit: identifier • LBRACE …`,
conflicting on `LBRACE` (shift continues struct_lit; reduce takes primary_expr). This
state is reached after `SWITCH simple_stmt SEMICOLON`, i.e. the SECOND `identifier` in
`switch init; v := x.(type)` — `type_switch_guard` becoming reachable from one more
grammar position re-exposes the exact same ambiguity Task 3 already classified and
proved lexically unreachable, for the same reason: the M10 bridge pushes SWITCH's
cond-frame at `SWITCH` itself (src/parser/lexer_bridge.c:209-210,
`m10_push_frame(M10_FRAME_KIND_SWITCH)`), not at the guard's position, so the
first depth-0 `{` after SWITCH — whether or not an init clause precedes the guard —
is unconditionally tokenized `LBRACE_BODY`, which `struct_lit` cannot consume. An
unparenthesized struct literal as an init-ed type-switch guard's operand is therefore
still a parse error, not a silent misparse; parenthesizing it still works (verified
below).

**Behavioral verification (compiled probes, `bin/goo`, real exit codes):**
- Full golden suite: 327 passed, 0 failed (up from 326; +1 new golden). `make test`:
  76 passed / 1 skipped. Both unchanged in shape from pre-task baseline.
- IR differential: `--emit-llvm` output for all 9 pre-existing switch/type-switch
  goldens (`switch_probe`, `tagless_switch_probe`, `type_switch_probe`,
  `type_switch_fmt_probe`, `type_switch_valptr_probe`, `string_switch_probe`,
  `iface_target_switch`, `rtti_type_switch_any`, `iface_print_typeswitch`) is
  byte-identical before/after this change (modulo the module-name label, which is
  derived from the `-o` output path, not source content) — the frozen-behavior
  requirement holds exactly, not just "still compiles."
- New forms all compile and run correctly: `switch x := 2; x { case 2: … }` (plain
  init); `switch a, b := 1, 2; a { … }` (tuple short-decl init, the shift side of
  Conflict 1); `switch a, b = 1, 2; a { … }` (tuple assignment init, the reduce side —
  LALR's single-token lookahead resolves this correctly at runtime despite the static
  shift/reduce conflict, because the conflict only delays the decision, it doesn't
  commit to one production; confirmed both forms parse identically whether reached via
  IF-init or SWITCH-init); `switch v := f(); w := v.(type) { case int: … }` (the
  spec's explicit acceptance criterion for type-switch-with-init, spec line 42).
- Init-var scoping verified end-to-end: visible in every case including `default`
  (`switch x := 9; x { default: fmt.Println(x) }` prints `9`); NOT visible after the
  switch (`switch x := 1; x { … }; fmt.Println(x)` rejects with `Type error …
  Undefined variable 'x'`, the exact if-init-scope-reject-probe wording, via the same
  wrapping-block desugar mechanism — no checker changes needed).
- Struct-lit-as-tag edge case unchanged: `switch (Point{X: 1}) { … }` still parses
  (parenthesized); the bare unparenthesized form was already a parse error before this
  task (Task 3's proof) and stays one.

`EXPECTED_SR` is 84 in `scripts/grammar-tripwire.sh`, updated in the same commit as
this ledger entry.
