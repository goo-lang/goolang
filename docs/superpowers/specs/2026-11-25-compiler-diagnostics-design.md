# Compiler diagnostics: one format, a source line, and a fix to paste (Design)

**Date:** 2026-11-25
**Status:** Draft, awaiting approval
**Approach:** A (chosen by the user from A/B/C, see "Approaches")

## 1. Intent

The reader of a Goo diagnostic is a programmer, human or LLM, who wants to
fix the bug. Today the compiler prints, for example:

```
Parse error at parse.goo:5:2: syntax error
Error: Parse failed
Type error at mismatch.goo:6:2: Cannot assign int64 to string
Error at codegen:0:0: Failed to emit object file: Permission denied
```

The parse error names no token, points at the line after the mistake, and
repeats itself. No diagnostic shows the source. Only a few messages (the error
union one) say how to fix the problem.

After this project every diagnostic looks like this:

```
union.goo:10:7: error: error union must be handled: use try, catch, or v, err := destructuring
   10 |     x := f()
      |          ^
  help: handle the error where you call f:
          x := try f()
          x := f() catch e { ... }
          x, err := f()
```

```
parse.goo:5:1: error: syntax error, unexpected '}', expecting expression
    5 | }
      | ^
```

The first line is the same shape for every diagnostic, so a person, an editor
and an LLM can all read the location and the message from it alone. The
source line and caret show where. The `help:` line, where there is one, gives
code that the reader can paste.

## 2. Goals

1. One printer for every diagnostic. The first line is
   `<file>:<line>:<col>: <severity>: <message>`, where severity is `error` or
   `warning`. A diagnostic with no position prints `goo: <severity>: <message>`.
2. Under the first line, the source line with its line number, and a caret
   under the column.
3. Optional `help:` lines. The first users are the diagnostics for the Goo
   extensions: error unions (`!T`, `try`, `catch`), nullable types (`?T`,
   `if let`), linear `qir.Qubit`, `comptime`, `arena`, `enum`/`match`. A help
   line contains code to paste, and uses the names from the user's own source
   where the checker has them.
4. Parse errors name the token found and the tokens expected
   (`%define parse.error detailed`), with readable token names through string
   aliases (`RBRACE "}"`). The duplicate `Error: Parse failed` line goes.
5. A new gate, `diag-format-probe`, holds the format.

**Scope of "every diagnostic" (design call, 2026-11-25):** every message that
the compiler prints to stderr for a user error or a user warning goes through
`diag_emit`. That covers the three functions above and the ~82 other compiler
`fprintf(stderr, ...)` sites that report an error or warning to the user. A
message without a source position uses the `goo: <severity>:` form. Out of
scope: the runtime (`src/runtime/`, which prints the PROGRAM's panics), and
debug output switched on by an environment variable (`GOO_ARC_DEBUG` and
similar).

## 3. Non-goals (this project)

- **Error codes** (`error[G0042]`, `goo explain`). Approach B. The printer takes
  a code argument from the start, so B can come later with no format change.
- **`--error-format=json`.** Approach C. Same reason.
- **Type names as the user wrote them** (`int`, "untyped int constant").
  Measured: `type_int(64)` names `int` and `int64` both `"int64"`
  (`src/types/types.c:85`), and the checker has no untyped-constant concept.
  Go treats `int` and `int64` as different types. A correct name therefore
  needs a type-identity change, which is a Go-compatibility project of its
  own. A display alias would print `int` for a declared `int64` too, which is
  worse than today.
- **"Did you mean" suggestions.**
- **Colour.**
- **Rewording existing messages.** The catalogue format strings stay as they
  are, except where a goal above needs a change (the parse error text).
- **New checks**, for example Go's "declared and not used" (Goo accepts
  `x := 1` with `x` unused today).

## 4. Anti-goals

- A format that a regex on the first line cannot read. Each diagnostic starts
  with exactly one `file:line:col: severity: message` line. Extra lines start
  with whitespace, so a line-based tool can tell them apart.
- A help line that is wrong. A help line that names a variable that is not in
  scope, or gives code that does not compile, is worse than none. Each help
  text must be proved by a fixture where the pasted fix compiles.
- A red reject suite. The 341 reject fixtures must stay green with no edits to
  their `.err.txt` sidecars, except for parse-error fixtures whose text the
  bison change rewords.

## 5. Constraints (measured 2026-11-25)

- **Reject fixtures match a substring.** Each `tests/golden/reject/*.err.txt`
  is one line, matched with `grep -qF` (`scripts/run_golden_reject.sh`). A new
  prefix or extra lines do not break them. A changed message text does.
- **`catalogue/diagnostics.tsv`** holds the message format strings (315 lines),
  kept equal to the source by `diagnostics-drift-probe`. Its id is a SHA-1 of
  file name and format, so a reworded message gets a new id.
- **The prefix has two dependents:** `tests/cli/cli_test.sh:246-247` look for
  the substrings `Parse error` and `Type error`. They change to the new form in
  the same commit as the printer. Measured: outside the three printers, no
  file in the tree reads `Type error at`, `Parse error at` or `Error at`.
  `tests/integration/error_reporting_test.c` does not build (deferred,
  Makefile:4992). There is no Haskell code in the tree yet, and
  `scripts/frontend_diff.sh` compares program dumps, not stderr. Other
  `cli_test.sh` cases match stderr substrings too: the builder checks each one
  against the new form.
- **Call sites:** `type_error` 355, `codegen_error` 442, `yyerror` 7. The
  printer goes behind the existing functions, so these sites do not change.
  About 82 other `fprintf(stderr, ...)` sites in `src/` (outside
  `src/runtime/`) print driver, I/O and import errors and some warnings.
- **Positions:** the lexer counts `Position.column` in BYTES, from 1
  (`src/lexer/lexer.c:72`). Bison gets each token's location in `yylloc`
  (`src/parser/lexer_bridge.c:514`, `:533`), so `yyerror` can report the
  location of the token that bison rejected without a bridge change.
- **Sources:** `lexer_new` is called at `src/compiler/goo.c:1218`, `:1231` and
  `src/parser/lexer_bridge.c:540`. Type checking runs after parsing, so the
  printer cannot assume the lexer or `sources[]` is still alive.
- **Grammar:** a change to `src/parser/parser.y` goes through the `goo-grammar`
  skill. `./scripts/grammar-tripwire.sh` must PASS with the recorded
  shift/reduce and reduce/reduce counts before and after. Bison is 3.8.2;
  `parse.error detailed` needs 3.6 or later.
- **Build gates:** `-Werror -Wpedantic`, C23. New code must not raise any
  (tool, rule, file) group in the gcc14/tidy18/cppcheck2.13 findings baseline
  (measure in the `ci` container, see `scripts/findings_probe.sh`).
- **A new gate** needs a `--self-test` (`probe-teeth-probe`), a
  `tools/parity-gate-count.txt` change, a `tests/probes/census.txt` change,
  and, if it adds a source file, a Bazel target.

## 6. Approaches

| | What | Trade-off |
|---|---|---|
| **A (chosen)** | One shared printer, source line and caret, help lines, parse-error detail. No codes. | About three core files change. Fixtures stay green. Gives the reader location, cause and a fix. No long-form lookup. |
| B | A, plus stable codes in the first line and `goo explain`. | About 315 call sites and a new catalogue column. The Haskell front end must emit the same codes. |
| C | B, plus JSON output. | Best for tools and an LSP. Adds little for an LLM reading text. |

The user chose A, because it gives the stated reader the largest gain for the
least cost, and leaves B and C open.

## 7. Design

### 7.1 The printer

A new unit, `src/errors/diagnostic.c` with `include/diagnostic.h`:

```c
typedef enum { DIAG_ERROR, DIAG_WARNING } DiagSeverity;

// code: NULL until approach B. help: NULL or one or more lines.
[[gnu::format(printf, 5, 6)]]
void diag_emit(DiagSeverity sev, Position pos, const char* code,
               const char* help, const char* format, ...);
void diag_vemit(DiagSeverity sev, Position pos, const char* code,
                const char* help, const char* format, va_list args);

// Keep a private copy of a file's text, so the printer can show a line
// after the lexer and the source buffer are gone.
void diag_register_source(const char* filename, const char* text, size_t len);
void diag_free_sources(void);
```

`type_error`, `type_warning`, `codegen_error` and `yyerror` call
`diag_vemit`. Their own signatures do not change. A new
`type_error_help(checker, pos, help, format, ...)` gives a help line, and it
counts the error the same way as `type_error`.

### 7.2 The first line

`<file>:<line>:<col>: error: <message>`. `pos.filename == NULL` or
`pos.line <= 0` prints `goo: error: <message>` (this replaces
`Error at codegen:0:0:`).

### 7.3 The source line and caret

If the registry holds the file and the line exists:

```
%5d | <the line, without its newline>
      | <pad>^
```

The column is a 1-based BYTE offset. The pad walks the bytes of the line
before the column: a tab stays a tab, a UTF-8 continuation byte (`10xxxxxx`)
adds nothing, and any other byte adds one space. So the caret lines up under
the right character for any terminal tab width and for multi-byte
characters. If the line is not available, the printer
prints the first line only.

### 7.4 Help lines

Each help text is one or more lines. The first starts with `  help: `, and
the rest are indented to line up. Code in a help line uses the user's names
where the checker has them (for example the callee name `f` above), and a
generic form where it does not.

The first set: the diagnostics in `catalogue/diagnostics.tsv` that belong to
the Goo extensions (60 rows mention try, catch, union, nullable, qubit,
comptime, arena, enum or linear). The builder picks the ones that a reader
hits most, starting with the unhandled error union, the use of a consumed or
dropped `qir.Qubit`, and a `?T` value used without a check.

### 7.5 Parse errors

- `%define parse.error detailed` in `parser.y`.
- String aliases for the punctuation and keyword tokens (`RBRACE "}"`,
  `FUNC "func"`, and so on), so bison prints `unexpected '}', expecting
  expression` rather than token names.
- `yyerror` passes the bison text to `diag_emit` at `yylloc`, the location of
  the token that bison rejected, not at `current_lexer->pos` (which can be
  past it, because the bridge reads ahead). The message names the token
  found, so a position on the next line still says what is wrong.
- `goo.c:1244` stops printing `Error: Parse failed`.
- The 7 explicit `yyerror("...")` messages keep their text.

### 7.6 Gate: `diag-format-probe`

Fixtures under `tests/diag_format/` and a script with
`--self-test`. It covers a parse error, a type error, a type error with help,
a codegen error and a diagnostic with no position. It checks:

- the first line of each diagnostic matches
  `^[^ :][^:]*:[0-9]+:[0-9]+: (error|warning): .+$`, or the no-position form;
- the source line and caret are present and the caret column is correct,
  including a line with tabs and a line with a multi-byte character before the
  error column, line 1, and a last line with no newline;
- each help fixture: the pasted fix compiles (the fixture has a fixed copy).

The self-test proves the gate goes red when the caret is off by one, when the
prefix changes, and when a help fix does not compile.

## 8. What the builder decides

- Where the registry lives and whether it copies or references the text, as
  long as a diagnostic after parsing still shows the line.
- Truncation of a very long source line.
- The exact help wording, inside the anti-goals above.
- Which parse-error reject fixtures need new `.err.txt` text after 7.5. Each
  one is listed in the PR, with the old and the new line.

## 9. Done when

- The goals in section 2 hold, and `diag-format-probe` and its self-test are
  in `verify-core`.
- The 341 reject fixtures and the golden suites pass. Only parse-error
  sidecars changed, and each change is listed.
- `grammar-tripwire.sh`, `diagnostics-drift-probe` and the findings gate (CI
  toolchain) pass.
