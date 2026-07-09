---
name: goo-grammar
description: Use BEFORE touching src/parser/parser.y, src/parser/lexer_bridge.c, src/lexer/lexer.c token emission, or any grammar/precedence/conflict work in the Goo compiler — and when diagnosing a bison conflict-count change, a misparse involving braces/brackets/newlines, or a parse error in a construct that "should" work. Carries the conflict-baseline tripwire, the stop rule for deltas, and the map of the grammar's LALR workaround sites.
---

# Goo grammar work

The grammar is LALR with a hand-maintained conflict baseline and several
context-sensitive lexer workarounds. Grammar changes are safe here ONLY when
run through this procedure — the failure mode otherwise is a silent parse
behavior change, not a build break.

## Procedure

1. **Before editing**: confirm a clean baseline.

   ```bash
   ./scripts/grammar-tripwire.sh    # expect: PASS (the exact counts recorded in
                                     # scripts/grammar-tripwire.sh's EXPECTED_SR/
                                     # EXPECTED_RR — see the ledger for the current
                                     # baseline number)
   ```

   If it fails BEFORE your edit, stop — the tree is already off-baseline;
   find out why before adding your change on top.

2. **Read the workaround map first** if your change involves braces after
   identifiers, `[]` in type position, type arguments to builtins, newline
   sensitivity, trailing commas, or spread:
   `references/workarounds.md` — one entry per trap, with verified file:line
   anchors and "extend this way / don't touch that" guidance.

3. **After editing**: tripwire again.
   - PASS → proceed to build + goldens (step 4).
   - FAIL → **STOP. Any delta is a stop-the-line event.** Do not rationalize
     a +1. Follow the justified-delta procedure in
     `references/conflict-ledger.md`: classify each new conflict via the
     counterexamples the script prints, prove parse behavior with the golden
     suite plus targeted probes for the exact token sequences involved, and
     only then update the script's `EXPECTED_SR`/`EXPECTED_RR` and the
     ledger's history table IN THE SAME COMMIT. If you cannot justify it,
     revert the grammar change. (Precedents: #92's justified +1; #108 proving
     a precedence move byte-identical; #111 shipping three grammar tasks at
     zero delta.)

4. **Build + verify** (real exit codes — never a pipeline's `$?`):

   ```bash
   make lexer                        # exit 0
   bash scripts/run_golden.sh        # N passed, 0 failed — count only ever grows
   make test                         # 76 passed / 1 skipped
   ```

5. **If you touched ANY header** (`include/ast.h` above all): `make clean`
   first — the Makefile has no header dependencies; stale objects silently
   miscompile. In `ast.h`, append enum values and struct fields at the TAIL
   only. If you added a field to a parser-allocated node, audit every
   `malloc(sizeof(<Node>))` site to initialize it — malloc garbage in a flag
   is a heisenbug factory (see the `has_spread` audit precedent).

6. **New reject behavior** gets a Makefile reject-probe (compile-must-fail +
   message grep) wired into the `verify:` list; new accept behavior gets a
   golden whose expected output is produced by `go run` on an equivalent
   program, never hand-written.

## References

- `references/conflict-ledger.md` — baseline history, justified-delta
  procedure, differential-verification recipe.
- `references/workarounds.md` — LBRACE_BODY bridge, RBRACKET_SLICE split,
  type_call_arg, struct-body ASI, COMMA-before-RBRACE arms, newline-blind
  func-result hazard, spread arm, header rules.
- Generic bison semantics (grammar syntax, %token/%left, LALR mechanics):
  the bison manual — https://www.gnu.org/software/bison/manual/ — not
  duplicated here.
