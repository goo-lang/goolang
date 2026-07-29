# Which row tables cover which arm of the shared escape engine

Measured 2026-07-29 against `ce6af49`, by `scripts/escape_arm_coverage.sh`.
Re-runnable: the script reads the arm list out of the source, so a new arm joins
the matrix without an edit here.

**Two states are recorded below.** "Before" is `ce6af49`, the baseline that
motivated the work. "After" is the same measurement with the seven rows this
change adds. Both are kept, because the delta is the evidence that the rows do
something.

Both arm populations are covered: `escape_expr_taint` (17 arms) and
`escape_walk_stmt` (20 arms).

| | Before | After |
|---|---|---|
| Expression arms precision-covered in all three suites | 3 of 17 | 6 of 17 |
| Expression arms soundness-covered in `local` (the pass T4 consumes) | 4 of 16 | 9 of 16 |
| Statement arms soundness-covered in `local` | 7 of 18 | 12 of 18 |
| Expression arms never reached by any fixture | 6 | 5 |
| Statement arms never reached by any fixture | 11 | 7 |
| Row counts (param / block / local) | 23 / 31 / 16 | 24 / 32 / 27 |

## Why this exists

PR #255 merged three hand-mirrored escape walks into one engine,
`src/types/escape_core.c`. During that work ONE arm was mutation-tested by
hand — `AST_POSTFIX_EXPR` — and only `local_escape`'s table noticed. The other
54 rows stayed green. `.handoff.md` recorded that as a ledger item.

The problem was never the postfix arm. It was that the arm we knew about was
the arm somebody happened to try. `escape_expr_taint` has 17 arms. This measures
all of them.

T4 emits `goo_release` for non-escaping locals. It is the first change in the
ARC arc that frees memory, so it is the first that can dangle a pointer, and it
consumes these three tables as its safety net. Measure the net before standing
on it.

## Method

The default arm of `escape_expr_taint` does not merely return all-taint — it
calls `escape_mark()` on the spot. So an ABSENT arm marks every slot escaping
the instant that construct appears. The script reproduces an absent arm by
injecting a guard at the top of the function rather than deleting a `case`
label, because the 17 arms have three different shapes and a `sed` deletion
would leave unreachable code or fall through into the previous arm's body — a
different mutation than the one intended.

**Two directions, because they test two different row populations.**

| Mode | Mutation | Makes the pass | Detected only by |
|---|---|---|---|
| `over` | arm behaves as absent: all-taint, marked | MORE conservative | a PRECISION row (expects `false`) |
| `under` | arm claims the value aliases nothing | LESS conservative | a SOUNDNESS row (expects `true`) |

A precision row cannot catch an `under` mutation, and a soundness row cannot
catch an `over` one. The two matrices are complementary, and the results below
prove it: `AST_FUNC_LIT` is covered in all three suites for soundness and in
none of param/local for precision, while `AST_SELECTOR_EXPR` in `local` is the
exact mirror.

`under` is the direction that matters most for T4. `escape_core.h` names
under-marking as "the ONLY bug class that can dangle a pointer".

### Instrument verification

`scripts/escape_arm_coverage.sh --self-test` runs six controls, and no cell of
either matrix should be believed until they pass:

| Control | Expected | Why it is needed |
|---|---|---|
| 1 | baseline: all three PASS | A red baseline makes every cell meaningless |
| 2 | `over`/`AST_POSTFIX_EXPR`: all three FAIL | Read `PASS PASS FAIL` at `ce6af49`, which reproduced PR #255 and was the ledger item. Closing the gap CHANGED the fact this control asserted, and the self-test caught that. It now guards param row 24 and block row 32 against deletion |
| 2b | `over`/`AST_UNARY_EXPR`: block FAIL only | The strongest control here, and the only one with a MIXED pattern. Controls 2-4 all expect every suite red, which a bluntly broken build also produces. A wrongly injected guard, or a build that ignores the injection, cannot produce `PASS FAIL PASS` by accident |
| 3 | `over`/`AST_IDENTIFIER`: all three FAIL | Proves the harness can turn every suite red |
| 4 | `under`/`AST_IDENTIFIER`: all three FAIL | Controls 1-3 say nothing about the `under` direction, which flips a different row population |
| 5 | `under`/`AST_TYPE_ASSERT`: all three PASS | A true negative, cross-checked against `--reach` reporting 0 hits |

**The failure mode guarded:** a mutation that does not COMPILE looks exactly
like a covered arm, and so does a crash. Every verdict is read from the suite's
own `summary:` line, never from an exit status alone, and a run with no summary
line is `INCONCLUSIVE` rather than `COVERED`.

**Equivalent mutants.** `under` replaces an arm with
`return escape_taint_new(n);`. An arm whose body already IS that line cannot be
detected by any row, so reporting `GAP` there would be a false finding.
`AST_LITERAL` is such an arm. The script computes this from the source rather
than hardcoding it.

## Matrix 1 — precision (`--over`)

Verdict `COVERED` means at least one row in that suite fails when the arm is
absent.

| Arm | param (23) | block (31) | local (16) |
|---|---|---|---|
| `AST_IDENTIFIER` | COVERED | COVERED | COVERED |
| `AST_LITERAL` | GAP | COVERED | COVERED |
| `AST_BINARY_EXPR` | GAP | GAP | COVERED |
| `AST_UNARY_EXPR` | GAP | COVERED | GAP |
| `AST_POSTFIX_EXPR` | GAP | GAP | COVERED |
| `AST_INDEX_EXPR` | GAP | GAP | GAP |
| `AST_SLICE_INDEX_EXPR` | GAP | GAP | GAP |
| `AST_SELECTOR_EXPR` | COVERED | COVERED | GAP |
| `AST_CALL_EXPR` | COVERED | COVERED | COVERED |
| `AST_FUNC_LIT` | GAP | COVERED | GAP |
| `AST_STRUCT_LITERAL` | GAP | COVERED | GAP |
| `AST_SLICE_EXPR` | GAP | GAP | GAP |
| `AST_ARRAY_LITERAL` | GAP | GAP | GAP |
| `AST_KEYED_ELEMENT` | GAP | GAP | GAP |
| `AST_PAREN_EXPR` | GAP | GAP | GAP |
| `AST_SLICE_CONVERSION` | GAP | GAP | GAP |
| `AST_TYPE_ASSERT` | GAP | GAP | GAP |

Only 3 arms of 17 are covered in all three suites.

## Matrix 2 — soundness (`--under`)

| Arm | param (23) | block (31) | local (16) |
|---|---|---|---|
| `AST_IDENTIFIER` | COVERED | COVERED | COVERED |
| `AST_LITERAL` | N/A-equivalent | N/A-equivalent | N/A-equivalent |
| `AST_BINARY_EXPR` | GAP | GAP | GAP |
| `AST_UNARY_EXPR` | GAP | COVERED | GAP |
| `AST_POSTFIX_EXPR` | GAP | GAP | GAP |
| `AST_INDEX_EXPR` | GAP | GAP | GAP |
| `AST_SLICE_INDEX_EXPR` | GAP | GAP | GAP |
| `AST_SELECTOR_EXPR` | COVERED | COVERED | COVERED |
| `AST_CALL_EXPR` | COVERED | COVERED | COVERED |
| `AST_FUNC_LIT` | COVERED | COVERED | COVERED |
| `AST_STRUCT_LITERAL` | GAP | GAP | GAP |
| `AST_SLICE_EXPR` | GAP | GAP | GAP |
| `AST_ARRAY_LITERAL` | GAP | GAP | GAP |
| `AST_KEYED_ELEMENT` | GAP | GAP | GAP |
| `AST_PAREN_EXPR` | GAP | GAP | GAP |
| `AST_SLICE_CONVERSION` | GAP | GAP | GAP |
| `AST_TYPE_ASSERT` | GAP | GAP | GAP |

**11 of the 16 testable arms have no protection against under-marking in any
suite.** That is the headline, and it is worse than matrix 1.

## Matrix 3 — reach (`--reach`)

How many times each arm actually RUNS during each suite. This separates two
causes that matrix 1 and 2 cannot tell apart: an arm reads `GAP` either because
no fixture contains that construct, or because the fixtures that contain it
carry only the other kind of row.

The probe does not return, so behaviour is unchanged and one build serves all
17 arms.

| Arm | param hits | block hits | local hits |
|---|---|---|---|
| `AST_IDENTIFIER` | 63 | 147 | 262 |
| `AST_LITERAL` | 0 | 36 | 44 |
| `AST_BINARY_EXPR` | 0 | 0 | 22 |
| `AST_UNARY_EXPR` | 4 | 19 | 8 |
| `AST_POSTFIX_EXPR` | 0 | 0 | 6 |
| `AST_INDEX_EXPR` | 0 | 0 | 0 |
| `AST_SLICE_INDEX_EXPR` | 0 | 0 | 0 |
| `AST_SELECTOR_EXPR` | 10 | 27 | 8 |
| `AST_CALL_EXPR` | 21 | 123 | 116 |
| `AST_FUNC_LIT` | 2 | 42 | 6 |
| `AST_STRUCT_LITERAL` | 4 | 18 | 8 |
| `AST_SLICE_EXPR` | 0 | 0 | 8 |
| `AST_ARRAY_LITERAL` | 0 | 0 | 0 |
| `AST_KEYED_ELEMENT` | 0 | 0 | 0 |
| `AST_PAREN_EXPR` | 0 | 0 | 8 |
| `AST_SLICE_CONVERSION` | 0 | 0 | 0 |
| `AST_TYPE_ASSERT` | 0 | 0 | 0 |

### The three matrices agree

A cross-check that could have failed and did not: an arm that never runs cannot
have its mutation detected, so every 0-hit cell MUST read `GAP` in both matrix 1
and matrix 2. Every one does. Conversely every `COVERED` cell has a non-zero hit
count. Three independently produced measurements, no contradiction.

## Findings

1. **`AST_INDEX_EXPR` has never been reached by any fixture in any suite.**
   This looked wrong against PR #255, whose headline was the map-key sink, so it
   was checked. It is correct: `assign_to_lvalue` handles `m[k] = v` through
   `mark_lvalue_subscripts` (sink #2b) and never routes through the
   `AST_INDEX_EXPR` arm of `escape_expr_taint`. That arm covers index reads in
   RVALUE position, and no fixture anywhere reads an element out of a map or a
   slice. Against the map holding 23.2% of the daemon's retained bytes, that is
   the most consequential hole in the table.

2. **`block_escape` is the best-covered of the three for precision, and
   `local_escape` the worst.** Arena eligibility made precision `block`'s
   purpose, so it carries precision rows. `param` and `local` were written
   mostly with soundness rows. T4 consumes `local`.

3. **Slice and map literals are reached but unguarded in both directions.**
   `AST_SLICE_EXPR` and `AST_PAREN_EXPR` (the map literal, a `MapLitNode`) run
   8 times each in `local_escape`, and neither a precision nor a soundness
   mutation of them is detected. `goo_slice_append` and `goo_map_set_sv` are
   30.8% of the daemon's retained bytes.

4. **Five arms are never reached and may be unreachable from the front end.**
   `AST_INDEX_EXPR`, `AST_SLICE_INDEX_EXPR`, `AST_ARRAY_LITERAL`,
   `AST_KEYED_ELEMENT`, `AST_SLICE_CONVERSION`, `AST_TYPE_ASSERT`. A fixture
   attempt is the only way to tell "no fixture" from "the parser does not make
   this node". Until that is done, do not record either cause as fact.

5. **The postfix ledger item is confirmed and its cause is now exact.** Neither
   `param_escape_test.c` nor `block_escape_test.c` contains a postfix expression
   at all — reach is 0, not merely undetected. And the arm has no soundness
   coverage in ANY suite, which the ledger did not record.

## After — the same three matrices with this change's rows

Rows added: `param` 24, `block` 32, `local` 17-22.

### Precision (`--over`), after

| Arm | param (24) | block (32) | local (22) |
|---|---|---|---|
| `AST_IDENTIFIER` | COVERED | COVERED | COVERED |
| `AST_LITERAL` | COVERED | COVERED | COVERED |
| `AST_BINARY_EXPR` | COVERED | COVERED | COVERED |
| `AST_UNARY_EXPR` | GAP | COVERED | GAP |
| `AST_POSTFIX_EXPR` | COVERED | COVERED | COVERED |
| `AST_INDEX_EXPR` | GAP | GAP | GAP |
| `AST_SLICE_INDEX_EXPR` | GAP | GAP | GAP |
| `AST_SELECTOR_EXPR` | COVERED | COVERED | GAP |
| `AST_CALL_EXPR` | COVERED | COVERED | COVERED |
| `AST_FUNC_LIT` | GAP | COVERED | GAP |
| `AST_STRUCT_LITERAL` | GAP | COVERED | GAP |
| `AST_SLICE_EXPR` | GAP | GAP | GAP |
| `AST_ARRAY_LITERAL` | GAP | GAP | GAP |
| `AST_KEYED_ELEMENT` | GAP | GAP | GAP |
| `AST_PAREN_EXPR` | GAP | GAP | GAP |
| `AST_SLICE_CONVERSION` | GAP | GAP | GAP |
| `AST_TYPE_ASSERT` | GAP | GAP | GAP |

**Four of these cells were not targeted.** `AST_LITERAL` in param, and
`AST_BINARY_EXPR` in param and block, closed because the two postfix rows
contain `i := 0` and `i < 3`. A fixture written for one arm carries others with
it, which is worth knowing before writing a row per arm: measure after each
addition rather than assuming one row buys one cell.

### Soundness (`--under`), after

| Arm | param (24) | block (32) | local (22) |
|---|---|---|---|
| `AST_IDENTIFIER` | COVERED | COVERED | COVERED |
| `AST_LITERAL` | N/A-equivalent | N/A-equivalent | N/A-equivalent |
| `AST_BINARY_EXPR` | GAP | GAP | COVERED |
| `AST_UNARY_EXPR` | GAP | COVERED | GAP |
| `AST_POSTFIX_EXPR` | GAP | GAP | GAP |
| `AST_INDEX_EXPR` | GAP | GAP | COVERED |
| `AST_SLICE_INDEX_EXPR` | GAP | GAP | GAP |
| `AST_SELECTOR_EXPR` | COVERED | COVERED | COVERED |
| `AST_CALL_EXPR` | COVERED | COVERED | COVERED |
| `AST_FUNC_LIT` | COVERED | COVERED | COVERED |
| `AST_STRUCT_LITERAL` | GAP | GAP | COVERED |
| `AST_SLICE_EXPR` | GAP | GAP | COVERED |
| `AST_ARRAY_LITERAL` | GAP | GAP | GAP |
| `AST_KEYED_ELEMENT` | GAP | GAP | GAP |
| `AST_PAREN_EXPR` | GAP | GAP | COVERED |
| `AST_SLICE_CONVERSION` | GAP | GAP | GAP |
| `AST_TYPE_ASSERT` | GAP | GAP | GAP |

### Reach, after

| Arm | param | block | local |
|---|---|---|---|
| `AST_IDENTIFIER` | 69 | 161 | 332 |
| `AST_LITERAL` | 4 | 44 | 76 |
| `AST_BINARY_EXPR` | 2 | 4 | 28 |
| `AST_UNARY_EXPR` | 4 | 19 | 8 |
| `AST_POSTFIX_EXPR` | 2 | 4 | 6 |
| `AST_INDEX_EXPR` | 0 | 0 | 14 |
| `AST_SLICE_INDEX_EXPR` | 0 | 0 | 0 |
| `AST_SELECTOR_EXPR` | 10 | 27 | 8 |
| `AST_CALL_EXPR` | 21 | 127 | 134 |
| `AST_FUNC_LIT` | 2 | 42 | 6 |
| `AST_STRUCT_LITERAL` | 4 | 18 | 14 |
| `AST_SLICE_EXPR` | 0 | 0 | 20 |
| `AST_ARRAY_LITERAL` | 0 | 0 | 0 |
| `AST_KEYED_ELEMENT` | 0 | 0 | 0 |
| `AST_PAREN_EXPR` | 0 | 0 | 14 |
| `AST_SLICE_CONVERSION` | 0 | 0 | 0 |
| `AST_TYPE_ASSERT` | 0 | 0 | 0 |

## What is still open, each with its measured cause

| Item | Cause, measured |
|---|---|
| `AST_POSTFIX_EXPR` has no SOUNDNESS coverage in any suite | Not a hole for the common shape. In Go `i++` is a STATEMENT, never an expression, and the plain expression-statement arm of `escape_walk_stmt` computes the taint and frees it WITHOUT marking. So under-marking the arm is unobservable for `i++`. It becomes observable only when the operand itself carries taint or has marking side effects (`arr[f(p)]++`), because the arm's recursion is what triggers those. A row would have to be contrived to that shape |
| `AST_SLICE_INDEX_EXPR`, `AST_ARRAY_LITERAL`, `AST_KEYED_ELEMENT`, `AST_SLICE_CONVERSION`, `AST_TYPE_ASSERT` never reached | Still 0 hits after this change. Whether the cause is "no fixture" or "the front end does not make this node" is UNDETERMINED — a fixture attempt is the only way to tell, and it was not done. `AST_KEYED_ELEMENT` has one known reason: the struct-literal arm walks `field_values` directly, so `&T{x: 1}` never produces a keyed-element node for the walk |
| `AST_UNARY_EXPR`, `AST_FUNC_LIT`, `AST_STRUCT_LITERAL` precision-GAP in param and local | Reached in both, so the fixtures exist and only a precision row is missing. Bounded, cheap, and not done here |
| `AST_SELECTOR_EXPR` precision-GAP in local | Reached 8 times. Same shape as the row above |
| Soundness GAPs in param and block for the five arms local now covers | The rows added here went into `local`, because `local` is the pass T4 consumes. param and block need their own |
| `escape_walk_stmt`'s 20 statement arms are unmeasured | This matrix covers `escape_expr_taint` only. The statement arms hold the SINKS, which is where under-marking does its damage, so this is the higher-value next matrix |

## The statement walk — the second arm population, and the worse one

`escape_walk_stmt` has 20 arms, and they are not the same kind of thing as the
expression arms. **An expression arm only PROPAGATES taint. A statement arm
DECIDES what escapes.** The arms ARE the sinks: `return`, `go f(x)`, assignment,
channel send. So the mutations mean something different:

| Mode | Mutation | Consequence |
|---|---|---|
| `stmt-over` | the arm behaves as absent: `escape_mark_all`, which is what the default arm does | more conservative |
| `stmt-under` | **the statement is SKIPPED ENTIRELY**, which DELETES A SINK | a skipped `return` never marks the returned local |

`stmt-under` is the highest-consequence mutation in this harness. Three arms
(`AST_SWITCH_STMT`, `AST_TYPE_SWITCH`, `AST_SELECT_STMT`) RECURSE into a nested
statement body, so skipping one leaves an entire body unwalked, and
`switch n { case 1: return x }` never marks `x` at all.

**Equivalent mutants here are arms that already do nothing.**
`AST_BREAK_STMT` (empty, falls through) and `AST_CONTINUE_STMT` (a bare
`break;`) cannot be made to do less. `--classify` prints the no-op verdict and
the normalised body for all 20 arms, so the rule can be checked rather than
trusted — see the instrument defect recorded below for why that mode exists.

### Statement soundness (`--stmt-under`)

Before this change, `local` covered 7 of 18 testable arms. After, 12.

| Arm | param | block | local before | local after |
|---|---|---|---|---|
| `AST_BLOCK_STMT` | COVERED | COVERED | COVERED | COVERED |
| `AST_EXPR_STMT` | COVERED | COVERED | COVERED | COVERED |
| `AST_IF_STMT` | GAP | COVERED | GAP | **COVERED** |
| `AST_IF_LET_STMT` | GAP | GAP | GAP | GAP |
| `AST_FOR_STMT` | GAP | GAP | COVERED | COVERED |
| `AST_RETURN_STMT` | COVERED | COVERED | COVERED | COVERED |
| `AST_GO_STMT` | COVERED | COVERED | COVERED | COVERED |
| `AST_DEFER_STMT` | GAP | COVERED | GAP | GAP |
| `AST_BREAK_STMT` | N/A | N/A | N/A | N/A |
| `AST_CONTINUE_STMT` | N/A | N/A | N/A | N/A |
| `AST_VAR_DECL` | COVERED | COVERED | COVERED | COVERED |
| `AST_CONST_DECL` | GAP | GAP | GAP | GAP |
| `AST_MULTI_ASSIGN` | GAP | GAP | GAP | **COVERED** |
| `AST_SWITCH_STMT` | GAP | GAP | GAP | **COVERED** |
| `AST_TYPE_SWITCH` | GAP | GAP | GAP | **COVERED** |
| `AST_SELECT_STMT` | GAP | GAP | GAP | **COVERED** |
| `AST_UNSAFE_STMT` | GAP | GAP | GAP | GAP |
| `AST_ARENA_BLOCK` | GAP | GAP | GAP | GAP |
| `AST_ASSERT_STMT` | GAP | GAP | GAP | GAP |
| `AST_ASSUME_STMT` | GAP | GAP | GAP | GAP |

The load-bearing sinks — `BLOCK`, `EXPR_STMT`, `RETURN`, `GO`, `VAR_DECL` — were
already covered in all three suites. That is the reassuring half.

### Statement precision (`--stmt-over`), after

| Arm | param | block | local |
|---|---|---|---|
| `AST_BLOCK_STMT` | COVERED | COVERED | COVERED |
| `AST_EXPR_STMT` | COVERED | COVERED | COVERED |
| `AST_FOR_STMT` | COVERED | COVERED | COVERED |
| `AST_RETURN_STMT` | COVERED | COVERED | GAP |
| `AST_VAR_DECL` | COVERED | COVERED | COVERED |
| every other arm | GAP | GAP | GAP |

Thin, and expected: `local` carries only four precision rows in total, so an
over-conservative mutation has almost nothing to break there.

### Statement reach, after

| Arm | param | block | local |
|---|---|---|---|
| `AST_BLOCK_STMT` | 59 | 199 | 200 |
| `AST_EXPR_STMT` | 40 | 108 | 144 |
| `AST_IF_STMT` | 0 | 4 | 6 |
| `AST_IF_LET_STMT` | 0 | 0 | 0 |
| `AST_FOR_STMT` | 2 | 4 | 6 |
| `AST_RETURN_STMT` | 13 | 14 | 52 |
| `AST_GO_STMT` | 4 | 18 | 6 |
| `AST_DEFER_STMT` | 0 | 4 | 0 |
| `AST_BREAK_STMT` | 0 | 0 | 0 |
| `AST_CONTINUE_STMT` | 0 | 0 | 0 |
| `AST_VAR_DECL` | 8 | 136 | 214 |
| `AST_CONST_DECL` | 0 | 0 | 0 |
| `AST_MULTI_ASSIGN` | 0 | 0 | 6 |
| `AST_SWITCH_STMT` | 0 | 0 | 6 |
| `AST_TYPE_SWITCH` | 0 | 0 | 6 |
| `AST_SELECT_STMT` | 0 | 0 | 8 |
| `AST_UNSAFE_STMT` | 0 | 0 | 0 |
| `AST_ARENA_BLOCK` | 0 | 61 | 0 |
| `AST_ASSERT_STMT` | 0 | 0 | 0 |
| `AST_ASSUME_STMT` | 0 | 0 | 0 |

### Statement findings

1. **`AST_MULTI_ASSIGN` was never reached by any of the 78 rows.** It is a sink,
   and `v, err := f()` is among the commonest statements in Go. `local` row 23
   closes it.

2. **`switch`, `type switch` and `select` were never reached either.** These are
   the recursing arms, so an unwalked body is the failure, not a missed
   propagation. Rows 24, 25 and 26.

3. **`AST_IF_STMT` had 0 hits in param and local.** An unwalked `then` branch is
   a broad hole rather than a narrow one. Row 27.

4. **`AST_ARENA_BLOCK` is reached 61 times in block yet reads `GAP`, and that is
   correct.** `block_escape` runs two passes: Pass 1 discovers units with its own
   walk, and Pass 2 drives `escape_walk_stmt` from the arena block's BODY. So
   this arm only ever sees a NESTED arena block, and no fixture has one. This was
   an incorrect assumption in a self-test control before it was checked — see
   below.

## Instrument defects found while building this, and what they cost

Recorded because each one produced a plausible-looking result.

1. **An empty extraction read as a finding.** `arm_body` matched `case AST_X:` as
   a complete line, but most statement arms are written `case AST_RETURN_STMT: {`
   with the brace on the label line. Those returned an EMPTY body. The `under`
   test asks "does the body equal `return escape_taint_new(n);`", which an empty
   body simply fails, so the bug was invisible for the expression walk. The
   `stmt-under` test asks "is the body empty?", and the same bug immediately
   called **15 of 20 arms no-ops**, including `AST_RETURN_STMT`. The matrix looked
   entirely plausible. Fixes: the extractor tolerates a brace on the label line,
   `assert_arm_found` refuses a label that does not exist, and `--classify`
   prints the verdict for every arm so the rule is checkable by eye.

2. **A self-test control asserted something false about the code.** Control 7
   expected `stmt-under/AST_ARENA_BLOCK` to move `block` only. It moved nothing,
   because of the two-pass structure in finding 4 above. The control was wrong,
   not the harness — and it was replaced with `AST_DEFER_STMT`, whose mixed
   pattern was found by MEASURING rather than by guessing again.

3. **A control encoded a historical fact that the work then changed.** Control 2
   asserted PR #255's result. Closing the postfix gap changed it. Re-baselining
   quietly would have destroyed the control's value, so it was re-pointed at the
   two new rows it now guards.

4. **`tee | head` truncated a saved matrix.** `head` exited, SIGPIPE killed
   `tee`, and the file held 5 of 17 rows. The same pipeline family this project
   has already been bitten by twice.

## Re-measured after the select-comm fix (PR follow-up to #266)

`escape_core`'s `AST_SELECT_STMT` arm used to hand `sc->comm` to
`escape_walk_stmt`. A select case's comm is an EXPRESSION, so it fell to the
walk's `default:`, which calls `escape_mark_all`. Every local in every function
containing a select therefore read as escaping. The fix routes comm through a
shared `escape_walk_expr_stmt` helper, which carries the channel-send sink with
it.

NO CELL REGRESSED. Both matrices were re-run against the tables above.

**Expression soundness (`--over`), deltas only:**

| Arm | param | block | local |
|---|---|---|---|
| `AST_LITERAL` | GAP -> **COVERED** | — | — |
| `AST_BINARY_EXPR` | GAP -> **COVERED** | GAP -> **COVERED** | — |
| `AST_POSTFIX_EXPR` | GAP -> **COVERED** | GAP -> **COVERED** | — |

Five cells gained coverage, and the cause is worth recording: `escape_mark_all`
was MASKING arm coverage. While every site was marked regardless, mutating an
expression arm could not change any verdict, so those cells read GAP. Once comm
is walked as an expression, the arms inside it matter and the existing rows can
see them. An over-conservative default does not merely cost precision — it hides
how little the suites actually test.

**Statement soundness (`--stmt-under`), deltas only:**

| Arm | param | block | local |
|---|---|---|---|
| `AST_SELECT_STMT` | GAP -> **COVERED** | GAP -> **COVERED** | COVERED |

This closes part of the standing ledger item "param_escape/block_escape lack the
five statement rows": `select` is now covered in all three suites. `MULTI_ASSIGN`,
`switch`, `type switch` and `if` remain GAP in param and block.

The new rows are `param_escape_test` row 25, `block_escape_test` row 33 and
`local_escape_test` row 28. Row 28 is the precision row and was measured to fail
on the old arm (`y` escapes=1) and pass on the new one.

## Re-measured after the self-store rule (PR B)

`assign_to_lvalue` now subtracts the lvalue base's taint before marking, for a
plain identifier base. It is shared by all three passes, so both expression
matrices were re-run against the tables above.

**NO CELL REGRESSED, in either matrix.** The soundness (`--under`) matrix came
back BYTE-IDENTICAL to the baseline, which is the result that matters for a
change to shared code — subtracting bits is the under-marking direction, and
under-marking is the class that dangles a pointer.

**Expression precision (`--over`), deltas only:**

| Arm | param | block | local |
|---|---|---|---|
| `AST_INDEX_EXPR` | — | — | GAP -> **COVERED** |
| `AST_PAREN_EXPR` | — | — | GAP -> **COVERED** |

Both cells come from ONE fixture, local row 29 (`m[s] = m[s] + 1`). This closes
the standing finding recorded above that `AST_INDEX_EXPR` had never been
reached for precision by any fixture in any suite: the arm's verdict is now
observable, because the self-store rule gives its result somewhere to matter.
`AST_PAREN_EXPR` came with it, since the map literal `map[string]int{}` is a
`MapLitNode` that runs through that arm.

The pattern from the earlier session repeats: one row bought two cells, and
neither was targeted. Measure after each addition rather than assuming one row
buys one cell.

### An instrument defect in this script — the header row lies

`matrix()` prints `| Arm | param (23) | block (31) | local (16) |` from a
HARDCODED string at `scripts/escape_arm_coverage.sh:460`. Those numbers are not
computed from the suites, and they have been wrong since the rows moved past
them — the real counts at the time of this run are param 25, block 33, local
32. The tables recorded above therefore carry whatever count the script had
baked in when they were pasted, which is why the parenthesised numbers disagree
between sections of this file.

The COVERED/GAP verdicts are unaffected: those come from real mutation runs.
Only the header is decorative. Fix it by computing the counts, or drop them.

## Reproducing

```sh
scripts/escape_arm_coverage.sh --self-test   # seven controls; run this first
scripts/escape_arm_coverage.sh --stmt-over   # statement precision
scripts/escape_arm_coverage.sh --stmt-under  # statement soundness (the sinks)
scripts/escape_arm_coverage.sh --reach-stmt  # statement reach
scripts/escape_arm_coverage.sh --classify    # the no-op rule, checkable by eye
scripts/escape_arm_coverage.sh --over        # matrix 1
scripts/escape_arm_coverage.sh --under       # matrix 2
scripts/escape_arm_coverage.sh --reach       # matrix 3
scripts/escape_arm_coverage.sh AST_INDEX_EXPR under   # one cell
```

The script restores `src/types/escape_core.c` on every exit path, including a
kill, and asserts the file is clean before it starts. No test-only code is
committed into the engine.

Not wired into a gate. This is evidence for which rows to write, not a
regression net.
