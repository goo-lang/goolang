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

This is not a way to look away. libFuzzer stops the whole run on the FIRST leak
it sees, so one small leak on an error path costs you every memory-corruption
finding that would have come after it — and corruption is what the fuzzer exists
to find.

Valid programs are leak-clean since finding 1 was fixed. Finding 2 (8 bytes on a
token bison discards at a syntax error) still fires, and the fuzzer reaches a
syntax error constantly, so leaks-on runs still stop early.

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

Two sessions, 2026-08-08, 5 minutes each on one core, before and after the
finding-1 fix:

| | Before fix | After fix |
|---|---|---|
| Executions | 467,735 | 497,202 |
| Rate | 3,963 / sec | 4,178 / sec |
| New corpus units | 4,704 | 4,769 |
| Crashes | **0** | **0** |
| Peak RSS | 2,069 MB | 2,066 MB |

Zero crashes across roughly a million executions is a genuine result and not a
null one: the parser survived that many mutations of its own fixture corpus
without a segfault, a UBSan report or an ASan memory error.

Both runs ended at libFuzzer's default `-rss_limit_mb=2048`. See the correction
under Findings — that limit is a knob, and NOT caused by a leak in this
project.

## Findings

### 1. 188 bytes leaked per assignment statement — FIXED 2026-08-08

`ast_node_free()` had no `case AST_EXPR_STMT`, so the node reached `default:`
and nothing freed its `expr` child. Linear in the statement count: one
assignment leaked 188 bytes, two leaked 376. Not specific to any operator —
`x := !5`, `x := -5`, `x := 1+2` and `y = x` all gave the same figure.

Fixed in `src/ast/ast.c` by adding the case, after checking that `expr` has a
single owner: the three construction sites (`parser.y:1219`,
`parser_actions.c:337` and `:701`) each build or receive it and store it nowhere
else, and the only readers borrow the pointer without freeing or re-parenting.

Gated by `ast-free-leak-probe` and `ast-free-leak-selftest`, both in
`verify-core`. The probe links a driver that parses AND frees in one process —
the compiler cannot see this class of defect, because it is a batch process that
exits without freeing. Teeth: removing the case from a scratch copy takes the
probe from 0 bytes to 21,600.

### A CORRECTION, recorded because it was published

An earlier version of this file said the leak above "blocks long runs" and was
"the ceiling on how long the tool can run". **That was wrong.** It was inferred
from the coincidence of a leak and an out-of-memory, and never measured.

Measured after the fix:

| | Before fix | After fix |
|---|---|---|
| Executions before OOM | 467,735 | 497,202 |
| Peak RSS | 2,069 MB | 2,066 MB |

The fix changed the peak by 3 MB. The out-of-memory is not caused by any leak
in this project:

- Individual inputs leak 0 bytes after the fix — valid programs, syntax errors,
  and 400-statement inputs with an error at the end, all measured at 0.
- Cutting `-max_len` from 65536 to 4096 changed the peak by 10 MB.
- Peak RSS against run count is SUB-LINEAR: 567 MB at 20k runs, 842 MB at 80k,
  1,055 MB at 200k. A per-execution leak would be linear.

The cause is libFuzzer's own corpus and feature tracking plus AddressSanitizer's
allocator overhead and quarantine, reaching libFuzzer's DEFAULT
`-rss_limit_mb=2048`. It is a knob, not a defect. Raise it, or treat ~500k
executions as one session.

### 2. ast_node_free() is missing 48 of 124 node types (open)

Found while fixing finding 1, by enumerating the `ASTNodeType` enum against the
cases in `ast_node_free()`. 71 of 124 members have a case. **48 do not**, and
each one falls to `default:`, which frees the node but not its children.

`AST_EXPR_STMT` was one of them and is now fixed. The rest include very common
constructs: `AST_IF_STMT`, `AST_FOR_STMT`, `AST_RETURN_STMT`, `AST_INDEX_EXPR`,
`AST_SELECTOR_EXPR`, `AST_PAREN_EXPR`, `AST_SELECT_STMT`, `AST_TYPE_SWITCH`.
`src/ast/ast.c:342` already records the `AST_SELECT_STMT`/`AST_SELECT_CASE` half
of this as a known gap.

Measured with the fuzz harness:

| Program | Leaked |
|---|---|
| `ch := make(chan int); _ = ch` | 201 bytes |
| one `select`, one case | 650 bytes |
| one `select`, two cases | 1,299 bytes |
| two `select`, two cases each | 2,196 bytes |

Roughly 649 bytes per select case.

**Severity is LOW for the shipped compiler and that is why it is still open.**
`bin/goo` is a batch process that exits, so it never frees the AST and these
leaks cost it nothing. They matter to callers that parse and free in one
process: this fuzzer, and any future long-running host. The REPL and LSP were
quarantined in P5.5.

**Not fixed here.** 48 cases, each needing its own ownership check — does any
other node hold this child, does any caller re-parent it — and a free-list
change is the class that produced the PR #278 use-after-free. Done hastily this
trades a harmless leak for a use-after-free. It wants its own arc, with the
`ast-free-leak-probe` fixture extended one construct at a time.

### 3. 8 bytes leaked per discarded token on a parse error (open)

Found while verifying the fix above. A malformed keyword — `pac1age main` —
leaks exactly 8 bytes.

```
make fuzz-parse
printf 'pac1age main\nfunc main(){ x := 1 }\n' > /tmp/e.goo
ASAN_OPTIONS=detect_leaks=1 ./bin/fuzz_parse /tmp/e.goo
```

`bridge_next_mapped` (`src/parser/lexer_bridge.c:407`) calls `xstrdup` on the
token text and hands the string to bison as a semantic value. When the parse
fails, bison discards the token without a `%destructor`, so the string leaks.

It does NOT scale: an input with 400 valid statements before the error leaks 0
bytes, because those tokens were consumed by successful reductions. Only the
tokens on bison's stack at the moment of failure leak.

**Not fixed here.** The fix is a `%destructor` in `src/parser/parser.y`, and
every change to that file goes through the `goo-grammar` skill and must hold the
conflict-count tripwire (`./scripts/grammar-tripwire.sh`) before and after. That
is its own change, not a footnote to this one.
