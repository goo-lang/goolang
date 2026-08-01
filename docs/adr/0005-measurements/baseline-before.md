# ADR 0005 baseline — every number, before the first line of code

Taken on `main` @ `15f666a`, tree clean, on branch `feat/arc-escape-reasons`
before any edit. ADR 0005 changes the SHARED escape engine, and two edits to
that engine in this leg produced a use-after-free. A change there is judged by
"did a recorded number move", so the numbers have to exist first.

Read this file as the thing tasks 3 and 4 must reproduce EXACTLY. Task 3 is a
mechanical widening of the accumulator and must move nothing here. Task 4 names
a cause at each mark and must also move nothing here, because `reasons != 0` is
the old boolean. The first number that moves belongs to task 8.

Toolchain: LLVM 22.1.8, valgrind-3.27.1, 32 cores.

## 1. The daemon retains 262,205 bytes

```bash
./bin/goo -O2 -o /tmp/daemon bench/daemon/daemon.goo    # THE PATH IS PART OF THE NUMBER
valgrind /tmp/daemon 2000
```

```
in use at exit: 262,205 bytes in 14,003 blocks
total heap usage: 94,004 allocs, 80,001 frees, 3,190,301 bytes allocated
ERROR SUMMARY: 0 errors from 0 contexts
```

**THE OUTPUT PATH CHANGES THE BYTE TOTAL, and it caught me in task 3.** `main`
reads `os.Args`, the runtime copies `argv[0]` to the heap, and nothing frees
it. A binary at a path three characters longer reports 262,208 rather than
262,205. Measured by copying ONE binary to two paths:

```
.../baseline/daemon      -> 262,205 bytes in 14,003 blocks
.../baseline/daemonXYZ   -> 262,208 bytes in 14,003 blocks
```

So compare against this file only from the SAME path, and read the BLOCK COUNT
first. 14,003 blocks is what a behaviour change moves. A byte total that drifts
while the block count holds is the path, not the compiler. The figures above
were taken at
`/tmp/claude-1000/-mnt-ssd2-Workspace-github-com-goolang/821b17b3-97f4-4672-bf13-8de782415627/scratchpad/baseline/daemon`,
which is why the snippet's `/tmp/daemon` will not reproduce them to the byte.

262,205 is item 1's recorded figure to three significant digits, so the ledger
in `.handoff.md` reconciles. **0 errors is half the baseline**: task 8 frees a
map key, and this line is what proves the freeing is new rather than pre-existing.

The target after task 8 is about 82,000 bytes. That is a PREDICTION from a
reverted spike, not a measurement of this tree.

## 2. The daemon's verdicts — `f` reads condition 1, not condition 6

```bash
GOO_ARC_DEBUG=1 ./bin/goo -o /dev/null bench/daemon/daemon.goo 2>&1 \
  | grep -E '^\[arc\?\] (handle|main):'
```

```
handle: req     -> RELEASE_NO_NO_BINDING
handle: fields  -> RELEASE_OK
handle: counts  -> RELEASE_OK
handle: total   -> RELEASE_OK
handle: i       -> RELEASE_NO_LOOP_SCOPE
handle: f       -> RELEASE_NO_ESCAPES        <- the 262,205
handle: n       -> RELEASE_NO_BLOCK_ESCAPE
handle: err     -> RELEASE_OK
handle: parts   -> RELEASE_OK
handle: i       -> RELEASE_NO_LOOP_SCOPE
main:   requests-> RELEASE_NO_NOT_OWNED
main:   n       -> RELEASE_OK
main:   err     -> RELEASE_OK
main:   last    -> RELEASE_OK
main:   i       -> RELEASE_NO_LOOP_SCOPE
```

`f -> RELEASE_NO_ESCAPES` settles a question two handoff blocks disagreed on.
The blocker is condition 1, so the reason set is what unblocks it. Condition 6
holds `n`, a different local, and it is out of scope for ADR 0005.

## 3. `make verify-core` — ALL GREEN in 99.40 s

| Gate | Baseline |
|---|---|
| `param_escape_test` | 166 assertions, 0 failed, 0/27 rows failed |
| `block_escape_test` | 112 assertions, 0 failed, 0/34 rows failed |
| `local_escape_test` | 43 assertions, 0 failed |
| `release_decision_test` | 197 assertions, 0 failed |
| `release-decision-teeth` | 9 of 9 CAUGHT |
| `escape-teeth` | 13 of 13 CAUGHT (param 5, block 5, local 3) |
| `test-golden` | 493 passed, 0 failed |
| `test-golden-o2` | 493 passed, 0 failed |
| `test-golden-reject` | 156 passed, 0 failed |
| `arena_routing_test` | 8 assertions, 0 failed |
| stdlib coverage | 178 symbols across 493 fixtures |

`include/escape_core.h` says "20 + 31 + 14 rows" for the three suites. That is
STALE — the suites print 27, 34 and 43. Do not quote the header.

The teeth entries, all CAUGHT:

- release_decision: `cond1-escapes`, `cond2-owned`, `cond3-arena`,
  `cond6-block-escape`, `loop-header`, `values-release-safe`, `alias-refusal`,
  `unreadable`, `self-append`
- param: `on-return`, `param-self-bit`, `retention-return`,
  `escapes-propagate`, `defer-precision`
- block: `discover-new`, `discover-addr-composite`, `site-source-slot`,
  `site-slot-miss`, `defer-is-like-go`
- local: `local-self-bit`, `local-slot-match`, `defer-is-like-go`

## 4. Arm coverage — 17 arms, and `AST_INDEX_EXPR` is covered by ONE table

```bash
./scripts/escape_arm_coverage.sh          # clean tree only: it writes to the tracked source
```

| Arm | param (23) | block (31) | local (16) |
|---|---|---|---|
| `AST_IDENTIFIER` | COVERED | COVERED | COVERED |
| `AST_LITERAL` | COVERED | COVERED | COVERED |
| `AST_BINARY_EXPR` | COVERED | COVERED | COVERED |
| `AST_UNARY_EXPR` | GAP | COVERED | GAP |
| `AST_POSTFIX_EXPR` | COVERED | COVERED | COVERED |
| `AST_INDEX_EXPR` | GAP | GAP | COVERED |
| `AST_SLICE_INDEX_EXPR` | GAP | GAP | GAP |
| `AST_SELECTOR_EXPR` | COVERED | COVERED | GAP |
| `AST_CALL_EXPR` | COVERED | COVERED | COVERED |
| `AST_FUNC_LIT` | GAP | COVERED | GAP |
| `AST_STRUCT_LITERAL` | GAP | COVERED | GAP |
| `AST_SLICE_EXPR` | GAP | GAP | GAP |
| `AST_ARRAY_LITERAL` | GAP | GAP | GAP |
| `AST_KEYED_ELEMENT` | GAP | GAP | GAP |
| `AST_PAREN_EXPR` | GAP | GAP | COVERED |
| `AST_SLICE_CONVERSION` | GAP | GAP | GAP |
| `AST_TYPE_ASSERT` | GAP | GAP | GAP |

**The row counts here (23/31/16) are NOT the suite row counts above (27/34/43).**
The matrix runs a subset. Compare a later run against THIS table, never against
section 3.

`AST_INDEX_EXPR` is the arm the SUBSCRIPT_STORE reason rides on, and only
`local_escape`'s table covers it. That is the table to extend in task 5, and it
is also why a param-side or block-side regression on this arm would go unseen.

## 5. The 15 marks, which is not the 21 the ADR claims

```bash
grep -nE 'escape_mark(_all)?\(ctx' src/types/escape_core.c    # 15 lines
```

Lines 186, 234, 247, 282, 359, 414, 468, 501, 507, 609, 689, 749, 821, 872
(`escape_mark`) and 970 (`escape_mark_all`). No file outside
`src/types/escape_core.c` calls either function, so the 15 are the whole
surface. ADR 0005 says 21 and is corrected by task 2.
