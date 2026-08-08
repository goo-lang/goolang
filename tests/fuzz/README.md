# Parser fuzzing

`fuzz_parse.c` is a libFuzzer harness over `parse_input()`, the Goo parser's
buffer entry point.

## Why

The parser is the one component that reads bytes somebody else wrote.
`make verify-core` runs 197 hand-written gates over 810 fixtures, and every one
of those fixtures was written by a person trying to express something. None of
them is trying to break the parser.

The coverage baseline (`docs/adr/0005-measurements/coverage-baseline.md`) put
the compiler at 58.1% branch / 56.5% MC/DC. A little under half of it has never
run.

## Running it

```
make fuzz-parse            # build bin/fuzz_parse (clang + libFuzzer + ASan + UBSan)
make fuzz-parse-smoke      # bounded run over the seed corpus, a few seconds
make fuzz-parse-run        # open-ended run; Ctrl-C to stop
make fuzz-parse-leaks      # same, with ASan leak detection ON (see below)
```

The seed corpus is built from `examples/*.goo` and `tests/golden/reject/*.goo`
(751 files) into `build/fuzz/seeds/`. Findings land in `build/fuzz/artifacts/`.

Not wired into `verify-core`, on purpose: a fuzz run has no fixed duration, and
`verify-core` is the per-change gate. `fuzz-parse-smoke` is bounded and belongs
in a pre-release sweep next to `make assert-corpus`. What `verify-core` holds
instead is the REJECT FIXTURE that each fixed finding becomes.

## Leak detection is OFF by default

`ASAN_OPTIONS=detect_leaks=0` for every target except `fuzz-parse-leaks`.

This is not a way to look away from a problem. `ast_node_free()` in
`src/ast/ast.c:189` carries a comment saying that some node types "fall to
`default:` below and leak their subtrees; a pre-existing gap, not introduced or
fixed by this task". With leak detection on, the fuzzer stops on the first seed
in the corpus and never reaches the memory-corruption findings that are the
reason it exists.

`fuzz-parse-leaks` is how you hunt that class deliberately.

## Triage rule

Decide this BEFORE a run, not while reading a crash.

1. **A crash counts when it reproduces on `bin/goo`**, the shipped gcc build. A
   finding that appears only under the clang sanitizer build is a separate,
   lower-priority item — record it, do not drop it.
2. **Minimise it**: `bin/fuzz_parse -minimize_crash=1 <artifact>`.
3. **Commit the reproducer** to `tests/fuzz/crashes/` with a one-line cause.
4. **The fix becomes a reject fixture** in `tests/golden/reject/`, so
   `verify-core` holds it afterwards. That, and not the fuzzer, is the
   regression gate.

Not in this cut: a hang, an out-of-memory caused BY AN INPUT, and any finding
that needs a stack deeper than the default limit.

Read that middle exclusion narrowly. It covers an input that makes one parse
allocate without bound. It does NOT cover an out-of-memory that our own leak
produces by accumulating across iterations — that one is not a property of the
input at all, it is the harness running out of room, and finding 1 below is
exactly that. Filing it under this exclusion would have hidden the single thing
that limits how long the tool can run.

## Measured

First real session, 2026-08-08, 5 minutes on one core:

| | |
|---|---|
| Executions | 467,735 |
| Rate | 3,963 / sec |
| New corpus units | 4,704 |
| Crashes | **0** |
| Ended by | out-of-memory at 2,069 MB |

Zero crashes in 467k executions is a genuine result and not a null one: the
parser survived half a million mutations of its own fixture corpus without a
segfault, a UBSan report or an ASan memory error. The run ended on the leak
below, not on a defect in the parser's logic.

## Findings

### 1. 188 bytes leaked per assignment statement (open, and it blocks long runs)

Found within 30 seconds of the first run, on a seed already in `examples/`.

```
package main
func main(){ x := 1; _ = x }
```

Reproduce:

```
make fuzz-parse
printf 'package main\nfunc main(){ x := 1; _ = x }\n' > /tmp/a.goo
ASAN_OPTIONS=detect_leaks=1 ./bin/fuzz_parse /tmp/a.goo
```

Linear in the number of assignment statements: one leaks 188 bytes, two leak
376. Not specific to any operator or literal — `x := !5`, `x := -5`,
`x := 1+2` and `y = x` all give the same figure.

Cause: `expression ASSIGN expression` (`src/parser/parser.y:1219`) builds an
`ExprStmtNode` wrapping a `BinaryExprNode`, and `ast_node_free()` has no
`case AST_EXPR_STMT`, so the node falls to `default:` and its `expr` child is
never freed. The 188 bytes are the `BinaryExprNode` (72), two identifier nodes
(112) and two interned strings (4). `src/ast/ast.c:189` already carries a
comment recording that some node types leak their subtrees this way — this
gives that comment a number and a reproducer.

**Severity: higher than it first appears.** In the compiler it is benign, a
batch process that exits. For this harness it is the ceiling: with leak
detection off the leaked trees still accumulate, so a session grows to the RSS
limit and libFuzzer reports an out-of-memory against whatever input happened to
be last. That is how the 5-minute run above ended. **Fixing it is what unlocks
runs longer than about 470k executions**, and it is therefore the highest-value
follow-up here.

**Not fixed in this change, deliberately.** The fix looks like a four-line case
following the `AST_UNSAFE_STMT` pattern, but a change to a free list is exactly
the class that produced the use-after-free in PR #278 — which shipped past
`verify-core`, 493 goldens and an 8/8 mutation-teeth run, and was caught only by
reading the diff. It needs its own change, its own review, and its own valgrind
probe. Bundling it into the commit that introduces the tool would put a memory
change where nobody is looking for one.

Until then, run in bounded batches (`-runs=N`) or raise `-rss_limit_mb`.
