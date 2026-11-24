# Compiler Diagnostics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every compiler diagnostic prints `file:line:col: severity: message`, then the source line with a caret, then optional paste-ready `help:` lines; parse errors name the token found and expected.

**Architecture:** One new unit, `src/errors/diagnostic.c`, owns the format and a registry of source texts. The existing printers (`type_error`, `type_warning`, `codegen_error`, `yyerror`) and the other user-facing `fprintf(stderr, …)` sites call it; their 804 call sites do not change. Bison's detailed error mode plus token string aliases give the parse-error text.

**Tech Stack:** C23, GNU Make, Bazel, Bison 3.8.2, bash probes.

**Spec:** `docs/superpowers/specs/2026-11-25-compiler-diagnostics-design.md` (approved 2026-11-25, commit ee8b5be4).

## Global Constraints

- C23. CFLAGS carry `-Werror -Wpedantic`: `make lexer` must print 0 diagnostics.
- First line, exact: `<file>:<line>:<col>: <severity>: <message>`, severity `error` or `warning`. No position (`filename == NULL` or `line <= 0`): `goo: <severity>: <message>`.
- Extra lines start with whitespace. Snippet: `%5d | <line>`, caret line `      | <pad>^`. Help: first line `  help: `, later lines indented to line up.
- `Position.column` is a 1-based BYTE offset (`src/lexer/lexer.c:72`). Pad rule: tab stays tab, UTF-8 continuation byte (`10xxxxxx`) adds nothing, any other byte adds one space.
- Out of scope (spec section 3): error codes (the `code` argument exists and is always NULL), JSON, colour, "did you mean", user-written type names, rewording existing messages, new checks.
- The 341 reject fixtures stay green with no `.err.txt` edit, except parse-error sidecars that Task 3 rewords; each such edit is listed in the PR with old and new text.
- `catalogue/diagnostics.tsv` and `diagnostics-drift-probe` stay green. If a task changes a format string, regenerate the catalogue in the same commit.
- Findings: no (tool, rule, file) group may rise in `scripts/findings-baseline.gcc14-tidy18-cppcheck2.13.tsv`. Measure in the `ci` podman container: `git archive HEAD | podman exec -i ci tar -x -C /w`, then in `/w` with `PATH=/t/bin:$PATH` (a `gcc` -> `gcc-14` link): `make CC=gcc-14 LLVM_CONFIG=/usr/bin/llvm-config-18 src/parser/parser.tab.c` and then `... findings-probe`. A new file's findings are new groups: fix them in code, do not adopt them.
- Grammar: any change to `src/parser/parser.y` follows the `goo-grammar` skill; `./scripts/grammar-tripwire.sh` PASSES with the recorded counts before and after.
- A new gate needs a `--self-test` mechanism, a `tools/parity-gate-count.txt` change and `LC_ALL=C ./tools/probe_census.sh > tests/probes/census.txt` in the same commit.
- Commit: `git -c commit.gpgsign=false commit`, conventional subject, imperative mood, trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Stage explicit paths only.
- `make verify-core` fails at `far-shim-probe` inside `.claude/worktrees/*` (socket path over 108 bytes). That is known; run the full net from a short path or in CI.

## Review Focus

1. A position past the end of the file (an error at EOF, or a line number beyond the last line): print the first line only, never read out of bounds. Test in Task 1.
2. CRLF source: the snippet must not print the `\r`. Test in Task 1.
3. An error in a second file of a package, or in a vendored `goostd/` file: the snippet comes from THAT file. Test in Task 2.
4. A column of 0, or a column past the end of the line: the caret goes at the end of the line; no overflow. Test in Task 1.
5. A file named with `./` or through `goo build <dir>`: the registry key must equal `pos.filename` as the lexer stored it. Test in Task 2.

---

### Task 1: The diagnostic unit

**Files:**
- Create: `include/diagnostic.h`, `src/errors/diagnostic.c`
- Create: `tests/unit/error/diagnostic_test.c`
- Modify: `Makefile` (`ERROR_SRCS` line 94, `GOO_ERROR_SRCS` line 136, a `diagnostic-test` target next to the other unit tests, `VERIFY_ALL_DEPS`), `src/errors/BUILD`, `include/BUILD`, `tests/unit/error/BUILD` (create if absent), `tools/parity-gate-count.txt`, `tests/probes/census.txt`

**Interfaces:**
- Produces (`include/diagnostic.h`):
  - `typedef enum { DIAG_ERROR, DIAG_WARNING } DiagSeverity;`
  - `[[gnu::format(printf, 5, 6)]] void diag_emit(DiagSeverity sev, Position pos, const char* code, const char* help, const char* format, ...);`
  - `void diag_vemit(DiagSeverity sev, Position pos, const char* code, const char* help, const char* format, va_list args);`
  - `void diag_register_source(const char* filename, const char* text, size_t len);` (stores a private copy; a second call for the same name replaces it)
  - `void diag_free_sources(void);`
  - `void diag_set_stream(FILE* out);` (test seam; NULL restores stderr)
  - `code` is accepted and ignored in this plan.

- [ ] **Step 1: Write the failing unit test** `tests/unit/error/diagnostic_test.c` with `goo_check.h`. Each case writes through `diag_set_stream(tmpfile())`, rewinds, and compares the whole output string:
  - `first_line_and_caret`: source `"package main\n\nfunc main() {\n    x := f()\n}\n"`, pos {line 4, col 10}, message `"m"` → `"t.goo:4:10: error: m\n    4 |     x := f()\n      |          ^\n"`.
  - `no_position`: filename NULL → `"goo: error: m\n"`; line 0 → same.
  - `warning_severity`: `DIAG_WARNING` → first line contains `": warning: m"`.
  - `tab_pad`: line `"\tx := 1"`, col 2 → caret line `"      | \t^"`.
  - `utf8_pad`: line `"π := y"`. π is 2 bytes, so `y` is at byte column 7. Expected caret line: `"      | "` + 5 spaces + `"^"` (π adds one space, `" := "` adds four).
  - `line_1` and `last_line_no_newline`: both show the right line.
  - `unregistered_file`: only the first line.
  - `past_eof` (Review Focus 1): line 99 of a 3-line file → only the first line.
  - `crlf` (Review Focus 2): source with `"\r\n"` endings → snippet line has no `\r`.
  - `col_out_of_range` (Review Focus 4): col 0 and col 500 on a 6-byte line → caret at column 1 and after the last byte; no crash.
  - `help_lines`: help `"use try f()\nor f() catch e { ... }"` → `"  help: use try f()\n        or f() catch e { ... }\n"` after the caret.
- [ ] **Step 2: Run it and see it fail**: `make diagnostic-test` → compile error, `diagnostic.h` not found.
- [ ] **Step 3: Implement `include/diagnostic.h` and `src/errors/diagnostic.c`.** Registry: a small growable array of {xstrdup name, copied text, len}, linear lookup by `strcmp`. Allocate through `xalloc.h` (the `alloc-doors-probe` refuses bare libc allocators). Count `\n` to find the line; strip a trailing `\r`.
- [ ] **Step 4: Run it and see it pass**: `make diagnostic-test` → all cases PASS.
- [ ] **Step 5: Wire the build.** Add the unit to `ERROR_SRCS` and `GOO_ERROR_SRCS`; add a Bazel `goo_cc_library` `//src/errors:diagnostic` (visibility: `//src/codegen`, `//src/parser`, `//src/types`, `//src/compiler`, `//tests/unit/error`), a header target `//include:diagnostic_h`, and a `goo_cc_test`. Add `diagnostic-test` to `VERIFY_ALL_DEPS`, bump the parity count by one, regenerate the census. Run `make lexer`, `bazel test //tests/unit/error/...`, `bash tools/parity_test.sh`, `make census-current-probe`: all rc=0.
- [ ] **Step 6: Measure findings** in the `ci` container (Global Constraints): PASS with 0 new.
- [ ] **Step 7: Commit** `feat(errors): add the diagnostic printer and source registry`.

### Task 2: Route the existing printers and register sources

**Files:**
- Modify: `src/types/types.c:1135-1160` (`type_error`, `type_warning`), `src/codegen/codegen_context.c` (`codegen_error`; remove `eprintf_checked` if it has no user left), `src/compiler/goo.c:1218` and `:1231`, `src/parser/lexer_bridge.c:540`, `tests/cli/cli_test.sh:246-247`
- Test: `tests/cli/cli_test.sh`, a new reject-style fixture pair for Review Focus 3 and 5

**Interfaces:**
- Consumes: `diag_vemit`, `diag_register_source` (Task 1).
- Produces: `type_error` and `codegen_error` keep their signatures; their output is the new format.

- [ ] **Step 1: Change the CLI expectations first.** In `cli_test.sh:246` expect `": error: syntax error"`; at `:247` expect `": error: "` plus the type message the case triggers. Add a case `snippet-on-type-error` that expects the `" | "` snippet line. Run `make test` → the changed and new cases FAIL.
- [ ] **Step 2: Add the Review Focus 3 and 5 tests** to `cli_test.sh`: (a) a two-file package directory where the type error is in the SECOND file, built with `goo build <dir>`: stderr's snippet shows a line of that file; (b) the same single file named as `./x.goo`: the snippet is present. Run → FAIL.
- [ ] **Step 3: Implement.** `type_error`/`type_warning`/`codegen_error` call `diag_vemit` (keep `error_count++`). Call `diag_register_source(fname, sources[fi], strlen)` next to each `lexer_new` at the three sites, using the same `filename` pointer string the lexer stores. Call `diag_free_sources()` at compiler exit.
- [ ] **Step 4: Run** `make test` (25+3 cases), `make test-golden`, `make test-golden-o2`, `make test-golden-reject`: all rc=0, and `git diff --stat tests/golden/reject` shows no change.
- [ ] **Step 5: Measure findings** in the `ci` container: 0 new.
- [ ] **Step 6: Commit** `feat(errors): print type and codegen errors through the diagnostic printer`.

### Task 3: Parse errors name the tokens

**Files:**
- Modify: `src/parser/parser.y` (prologue `%define`, the `%token` lines 132-141 and the other `%token` lines), `src/parser/parser_errors.c:242-250` (`yyerror`), `src/compiler/goo.c:1244`
- Modify: the parse-error sidecars under `tests/golden/reject/` that the new text breaks (listed in the commit body)

**Interfaces:**
- Consumes: `diag_emit` (Task 1). `yylloc` (`YYLTYPE`, `first_line`/`first_column`) as set in `src/parser/lexer_bridge.c:514,533`.

- [ ] **Step 1: Load the `goo-grammar` skill.** Run `./scripts/grammar-tripwire.sh` and record the PASS line (baseline counts).
- [ ] **Step 2: Write the failing test**: a cli case `parse-error-names-token` on `"package main\n\nfunc main() {\n\tx := 1 +\n}\n"` that expects stderr to contain `":5:1: error: syntax error, unexpected '}'"` and NOT to contain `"Parse failed"`. Run `make test` → FAIL.
- [ ] **Step 3: Implement.** `%define parse.error detailed`; a string alias on every punctuation and keyword token (`RBRACE "}"`, `FUNC "func"`, …; names for literal tokens such as `IDENTIFIER "identifier"`). `yyerror` reports at `yylloc.first_line/first_column` with the file name from `current_lexer`, through `diag_emit(DIAG_ERROR, …, NULL, NULL, "%s", msg)`. Delete the `Error: Parse failed` print.
- [ ] **Step 4: Run** `./scripts/grammar-tripwire.sh` → PASS with the same counts as Step 1. Any delta stops the task (goo-grammar stop rule).
- [ ] **Step 5: Run** `make test`, `make test-golden`, `make test-golden-o2` and `make test-golden-reject`. For each failing parse-error sidecar, set its text to a substring of the new message that still names the mistake; record old → new in the commit body.
- [ ] **Step 6: Measure findings** (the generated `parser.tab.c` is outside the gate; `parser_errors.c` is inside): 0 new.
- [ ] **Step 7: Commit** `feat(parser): name the unexpected and expected tokens in parse errors`.

### Task 4: The other user-facing messages

**Files:**
- Modify: the `src/` files outside `src/runtime/` that print a user error or warning with `fprintf(stderr, …)` or `fputs(…, stderr)`. Find them with `grep -rn -E 'fprintf\(stderr|fputs\(.*stderr' src --include='*.c' | grep -v '^src/runtime/'` (about 82 on 2026-11-25).

**Interfaces:**
- Consumes: `diag_emit` (Task 1).

- [ ] **Step 1: Classify every hit** into a table in the commit body: user error, user warning, or excluded (debug output behind an environment variable such as `GOO_ARC_DEBUG`, usage/help text on purpose, `--emit-*` payloads). Only the first two move.
- [ ] **Step 2: Write the failing test**: cli case `no-position-error-form` on a missing input file: stderr's first line starts with `"goo: error: "`. Update any existing cli case whose expected substring the new form changes. Run `make test` → FAIL.
- [ ] **Step 3: Implement** each move as `diag_emit(DIAG_ERROR or DIAG_WARNING, pos, NULL, NULL, …)`. Use a real `Position` where the site has one, else a zero `Position`.
- [ ] **Step 4: Run** `make test`, `make test-golden`, `make test-golden-o2`, `make test-golden-reject`: rc=0, no reject sidecar change.
- [ ] **Step 5: Measure findings**: 0 new.
- [ ] **Step 6: Commit** `refactor(errors): send the remaining user-facing messages through diag_emit`.

### Task 5: Help lines for the Goo extensions

**Files:**
- Modify: `src/types/types.c` and `include/types.h` (new `type_error_help`), `src/types/type_checker.c:2769` (error union), `src/types/qir_ownership.c` (catalogue rows c6d92077, e45648f5), `src/types/ownership_checker.c` (rows bd7b7685, b8860c84)
- Create: fixtures under `tests/probes/src/diag-format-probe/help/`: for each help, `<name>.goo` (the error) and `<name>.fixed.goo` (the help applied)

**Interfaces:**
- Produces: `[[gnu::format(printf, 4, 5)]] void type_error_help(TypeChecker* checker, Position pos, const char* help, const char* format, ...);` in `include/types.h`; counts the error like `type_error`.
- Consumes: `diag_vemit` (Task 1).

- [ ] **Step 1: Write the fixtures and a failing check**: for each of the five diagnostics, `goo build <name>.goo` stderr contains `"  help: "`, and `goo build <name>.fixed.goo` exits 0. Run → the help check FAILS.
- [ ] **Step 2: Implement `type_error_help`** and the five help texts. The message format string stays unchanged (catalogue rows keep their ids). Each help names the user's identifier where the site has it (the callee for the error union, the qubit variable for the qir rows, the variable for the nullable rows).
- [ ] **Step 3: Run** the check → PASS; `diagnostics-drift-probe` → PASS (regenerate the catalogue in this commit if `extract_diagnostics.py` now sees `type_error_help` as a new emitter: teach it the new function name).
- [ ] **Step 4: Run** `make test-golden`, `make test-golden-o2`, `make test-golden-reject`, `make test`: rc=0.
- [ ] **Step 5: Measure findings**: 0 new.
- [ ] **Step 6: Commit** `feat(types): help lines with paste-ready fixes for five extension errors`.

### Task 6: The `diag-format-probe` gate

**Files:**
- Create: `scripts/diag_format_probe.sh`, fixtures under `tests/probes/src/diag-format-probe/` (parse error, type error, help case from Task 5, codegen error, no-position error, a tab line, a multi-byte line)
- Modify: `Makefile` (`diag-format-probe`, `diag-format-selftest`, both in `VERIFY_ALL_DEPS`), `tools/parity-gate-count.txt` (+2), `tests/probes/census.txt`, `CLAUDE.md` (one paragraph: the diagnostic format and its gate)

**Interfaces:**
- Consumes: the compiler output of Tasks 2-5.

- [ ] **Step 1: Write the self-test first.** `--self-test)` arm: build mutants of a captured stderr (caret moved one column, prefix `Type error at` restored, a help fixture whose `.fixed.goo` does not compile) and require each one RED for its own reason; the unchanged control GREEN. Run → FAIL (no probe body yet).
- [ ] **Step 2: Implement the probe**: for each fixture, the first line matches `^[^ :][^:]*:[0-9]+:[0-9]+: (error|warning): .+$` or `^goo: (error|warning): .+$`; the snippet line and the caret column equal the fixture's recorded expectation; each help fixture's `.fixed.goo` compiles.
- [ ] **Step 3: Run** `make diag-format-probe diag-format-selftest probe-teeth-probe census-current-probe` and `bash tools/parity_test.sh`: all rc=0.
- [ ] **Step 4: Run the full net** from the main checkout path or let CI run it: `make verify-core` rc=0.
- [ ] **Step 5: Commit** `test(errors): gate the diagnostic format`.
