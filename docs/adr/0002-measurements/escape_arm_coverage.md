# Which row tables cover which arm of the shared escape engine

Measured 2026-07-29 against `ce6af49`, by `scripts/escape_arm_coverage.sh`.
Re-runnable: the script reads the arm list out of the source, so a new arm joins
the matrix without an edit here.

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

`scripts/escape_arm_coverage.sh --self-test` runs five controls, and no cell of
either matrix should be believed until they pass:

| Control | Expected | Why it is needed |
|---|---|---|
| 1 | baseline: all three PASS | A red baseline makes every cell meaningless |
| 2 | `over`/`AST_POSTFIX_EXPR`: local FAIL, param+block PASS | Reproduces PR #255 exactly. A different answer means the guard is not equal to an absent arm |
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

## Reproducing

```sh
scripts/escape_arm_coverage.sh --self-test   # five controls; run this first
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
