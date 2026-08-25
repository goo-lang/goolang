# Bazel Migration — Phase 2 (the front end) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the whole LLVM-free front end under Bazel — bison, lexer, parser, AST and the type checker — and port the four `tests/unit/types` suites that phase 1 could not, taking the parity count from 215 to 211.

**Architecture:** A `genrule` in its own package runs bison; the grammar conflict tripwire becomes an `sh_test`. The front-end libraries are built with dependency sets discovered per target rather than copied from `$(SRC_OBJS)`. Nothing is removed from the Makefile.

**Tech Stack:** Bazel 9.2.0, `rules_cc` 0.2.17, `rules_shell` 0.6.1, bison 3.8.2, GCC (gcc-14 on CI), C23. No LLVM in this phase.

**Spec:** `docs/superpowers/specs/2026-08-25-bazel-migration-design.md`
**Predecessor:** `docs/superpowers/plans/2026-08-25-bazel-phase0-1-foundation.md` (phases 0 and 1, PR #321)

## Global Constraints

Everything in the phase 0–1 plan's Global Constraints still applies. Read that section — commit form, backgrounded git, zsh `$pipestatus`, the `user.email` prohibition, `make verify-core` staying green. In addition:

- **The generated parser goes in its OWN package, `//src/parser/gen`.** `src/parser/parser.tab.c` and `parser.tab.h` are **gitignored and untracked**, but Make writes them to disk. A `genrule` in `//src/parser` declaring `outs = ["parser.tab.c"]` would collide with that on-disk file — but only on a machine where `make` has run, and not on a clean checkout. A build that behaves differently depending on whether `make` ran is exactly what this migration exists to remove.
- **bison is a host tool, like `llvm-config`.** No version is pinned, because `scripts/grammar-tripwire.sh` already is the version guard: the conflict counts are bison-version-sensitive, so a bison change that alters them fires the tripwire.
- **The grammar conflict baseline is `EXPECTED_SR=31`, `EXPECTED_RR=0`.** Per `CLAUDE.md` and the `goo-grammar` skill, any delta is stop-the-line. Nothing in this phase edits `parser.y`, so the count must not move at all.
- **This phase adds NO LLVM dependency.** LLVM enters through `include/codegen.h` and `include/codegen_cfctx.h` only, reaching the 14 files in `src/codegen/` plus `src/compiler/goo.c`. `bazel query 'somepath(//src/..., @llvm//:llvm_c)'` must stay empty for every target this phase adds, checked against a positive control.
- **The linker is `ld.lld`, which prints `undefined symbol: sym`,** not GNU ld's ``undefined reference to `sym'``. Grep for `ERROR` and read the real text before pattern-matching it. A discovery iteration in phase 1 reported "no undefined refs" while the link had failed.
- **Prove a mutation landed before interpreting the result.** A `sed` in phase 1 silently did nothing (an unescaped `|` collided with the `s|||` delimiter) and the resulting PASS read as "no teeth". Every mutation step here prints the mutated line and diffs against the backup.
- **Never infer a fact you can state.** Use `bazel aquery` for "is this an input", `bazel query somepath` for "does this reach LLVM", and a positive control for every negative result. Six of phase 0's eight defects were instruments that returned a clean answer while measuring nothing.

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `src/parser/gen/BUILD` | `genrule` running bison; exports the generated `.c`/`.h` | 1 |
| `src/parser/BUILD` | `exports_files(["parser.y"])`, then the parser `cc_library` | 1, 4 |
| `tools/grammar_tripwire_test.sh` | Wraps `scripts/grammar-tripwire.sh` for Bazel runfiles | 2 |
| `tools/BUILD` | `sh_test` for the tripwire | 2 |
| `src/lexer/BUILD` | `cc_library` over `LEXER_SRCS` | 3 |
| `src/ast/BUILD` | `cc_library` over `AST_SRCS` | 3 |
| `src/errors/BUILD` | `cc_library` over `ERROR_SRCS` | 5 |
| `src/comptime/BUILD` | `cc_library` over `COMPTIME_SRCS` | 5 |
| `src/package/BUILD` | `cc_library` over `PACKAGE_SRCS` | 5 |
| `src/types/BUILD` | extended: the full `GOO_TYPES_SRCS` front-end library | 5 |
| `tests/unit/types/BUILD` | four `cc_test` targets | 6, 7 |

---

### Task 1: The bison genrule

**Files:**
- Create: `src/parser/gen/BUILD`
- Create: `src/parser/BUILD`

**Interfaces:**
- Consumes: `//tools:defs.bzl` and `//include:prelude` from phase 0.
- Produces: `//src/parser/gen:parser_tab_c` and `//src/parser/gen:parser_tab_h`, plus a `cc_library` `//src/parser/gen:parser_tab` that carries the generated header on its include path. Task 4 depends on it.

**Background.** `Makefile:225` uses a GNU Make 4.3 grouped target (`&:`) so one bison run produces both outputs; its comment explains that without it, `-j` can invoke bison twice. A Bazel `genrule` with two `outs` is scheduled once by construction, so the workaround disappears.

- [ ] **Step 1: Confirm the collision this task avoids**

```bash
ls -la src/parser/parser.tab.c src/parser/parser.tab.h 2>/dev/null || echo "absent (clean checkout)"
git check-ignore -v src/parser/parser.tab.c
```
Expected: the files are present if `make` has run, and `.gitignore:11` matches. Both facts together are why the genrule lives in `src/parser/gen/` and not `src/parser/`.

- [ ] **Step 2: Create `src/parser/BUILD` exporting the grammar**

```python
# The grammar is the genrule's input; the generated sources live in the
# sibling package //src/parser/gen. They are NOT generated here on purpose:
# src/parser/parser.tab.c is gitignored but Make writes it to disk, so a
# genrule declaring that name as an output would collide with a real file --
# on developer machines only, never on a clean checkout.
exports_files(["parser.y"])
```

- [ ] **Step 3: Create `src/parser/gen/BUILD`**

```python
load("@rules_cc//cc:defs.bzl", "cc_library")

# One bison invocation, two outputs.
#
# Makefile:225 needs a GNU Make 4.3 grouped target (`&:`) to express this, and
# its comment records why: without it, `-j` can run bison twice concurrently
# for the two outputs. A genrule with two `outs` is scheduled once by
# construction, so the workaround is not needed here.
#
# bison -d writes the header next to the -o target, so naming parser.tab.c is
# enough to place parser.tab.h beside it.
genrule(
    name = "bison",
    srcs = ["//src/parser:parser.y"],
    outs = [
        "parser.tab.c",
        "parser.tab.h",
    ],
    cmd = "bison -d -o $(location parser.tab.c) $(location //src/parser:parser.y)",
    message = "Running bison on parser.y",
)

# The generated header, on an include path. parser_actions.c and
# lexer_bridge.c both spell it #include "parser.tab.h", so the directory
# containing it must be on the search path rather than the file being passed
# by location.
cc_library(
    name = "parser_tab",
    hdrs = ["parser.tab.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
)

# The generated implementation, exposed as a source for //src/parser to
# compile with the rest of the parser's translation units.
filegroup(
    name = "parser_tab_c",
    srcs = ["parser.tab.c"],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 4: Run bison through Bazel and confirm both outputs appear**

```bash
bazel build //src/parser/gen:bison > /tmp/bison.log 2>&1; echo "EXIT=$?"
ls -la bazel-bin/src/parser/gen/parser.tab.c bazel-bin/src/parser/gen/parser.tab.h
```
Expected: exit 0 and both files present. If bison is missing, this fails here rather than inside a compile action.

- [ ] **Step 5: Confirm ONE bison run produced both**

A genrule with two outputs must not be running bison twice. Ask the action graph rather than assuming:
```bash
bazel aquery 'mnemonic("Genrule", //src/parser/gen:bison)' --output=text 2>/dev/null \
  | grep -cE '^action '
```
Expected: `1`. A `2` would mean the outputs were split into separate actions and the Makefile's race has been reproduced rather than removed.

- [ ] **Step 6: Confirm the generated header is reachable by its bare name**

```bash
bazel query 'deps(//src/parser/gen:parser_tab)' --noshow_progress 2>/dev/null | grep -c 'parser.tab.h'
```
Expected: at least `1`.

- [ ] **Step 7: Confirm Bazel's copy is independent of Make's**

The whole point is that the Bazel build does not read the gitignored on-disk file:
```bash
bazel aquery 'mnemonic("Genrule", //src/parser/gen:bison)' --output=text 2>/dev/null \
  | grep -E 'Inputs:' | head -1
```
Expected: the inputs list names `src/parser/parser.y` and NOT `src/parser/parser.tab.c`.

- [ ] **Step 8: Commit**

```bash
git add src/parser/BUILD src/parser/gen/BUILD
git -c commit.gpgsign=false commit -m "build(bazel): run bison as a two-output genrule

Makefile:225 needs a GNU Make 4.3 grouped target to stop -j invoking bison
twice for the two outputs. A genrule with two outs is scheduled once by
construction, so the workaround is not needed; confirmed by aquery reporting
exactly one Genrule action.

The generated sources live in //src/parser/gen, not //src/parser, and that
placement is load-bearing. src/parser/parser.tab.c is gitignored but Make
writes it to disk, so a genrule declaring that name in //src/parser would
collide with a real file on developer machines and not on a clean checkout --
a build that behaves differently depending on whether make has run.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: The grammar conflict tripwire as a Bazel test

**Files:**
- Create: `tools/grammar_tripwire_test.sh`
- Modify: `tools/BUILD`

**Interfaces:**
- Consumes: nothing from Task 1 — it runs bison itself, on a copy, exactly as the Makefile's `grammar-tripwire` target does.
- Produces: `//tools:grammar_tripwire_test`, which claims the `grammar-tripwire` gate if that name is in `VERIFY_ALL_DEPS`.

**Background.** `scripts/grammar-tripwire.sh` asserts `EXPECTED_SR=31` and `EXPECTED_RR=0` exactly, exits 0/1/2, and already reads bison's status without a pipe. It takes the grammar path as `$1`, which is what makes it wrappable for runfiles.

- [ ] **Step 1: Confirm the gate name before naming the target**

```bash
grep -c 'grammar-tripwire' /tmp/all_deps.txt 2>/dev/null || \
  ./tools/parity.sh --list-make-gates | grep -c 'grammar'
```
If the count is 0, `grammar-tripwire` is not in `VERIFY_ALL_DEPS` and this test claims no gate. Record which it is — it decides whether the parity count moves in Task 9.

- [ ] **Step 2: Write the wrapper**

```sh
#!/usr/bin/env bash
# Runs the grammar conflict tripwire under Bazel.
#
# scripts/grammar-tripwire.sh already does the real work and already reads
# bison's exit status directly rather than through a pipeline. This wrapper
# exists only to locate the script and the grammar inside runfiles and to pass
# the grammar path as $1, which the script accepts.
#
# Exit codes pass straight through: 0 baseline exact, 1 delta (STOP), 2 bison
# itself failed.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

exec bash scripts/grammar-tripwire.sh src/parser/parser.y
```

Make it executable: `chmod +x tools/grammar_tripwire_test.sh`

- [ ] **Step 3: Register it**

Append to `tools/BUILD`:
```python
# The grammar conflict baseline, as a Bazel test.
#
# CLAUDE.md and the goo-grammar skill both make a conflict-count delta
# stop-the-line, so this is a gate rather than a report. It runs bison itself,
# on a scratch copy, and never consumes //src/parser/gen -- the baseline must
# be checked against the grammar as written, not against whatever the genrule
# last produced.
sh_test(
    name = "grammar_tripwire_test",
    size = "small",
    srcs = ["grammar_tripwire_test.sh"],
    data = [
        "//src/parser:parser.y",
        "//:grammar_tripwire_script",
    ],
)
```

And in the root `BUILD`, export the script so the test can take it as data:
```python
exports_files([
    "Makefile",
])

filegroup(
    name = "grammar_tripwire_script",
    srcs = ["scripts/grammar-tripwire.sh"],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 4: Run it**

```bash
bazel test //tools:grammar_tripwire_test --test_output=all > /tmp/gt.log 2>&1; echo "EXIT=$?"
grep -E 'grammar-tripwire:|PASSED|FAILED' /tmp/gt.log | head -3
```
Expected: `grammar-tripwire: PASS (31 S/R + 0 R/R — baseline exact)` and PASSED.

If the wrapper cannot find `scripts/grammar-tripwire.sh` in runfiles, the `data` entry is wrong — phase 0 verified that a script in `tools/` resolves the repo root as `$(dirname "$BASH_SOURCE")/..` and reads a root-level `data` file correctly, so follow that shape rather than inventing a new one.

- [ ] **Step 5: Prove it can report the delta**

A tripwire that cannot fire is not a tripwire. Mutate the BASELINE rather than the grammar — editing `parser.y` risks leaving a real grammar change behind, and the point is to test the comparison:

```bash
cp scripts/grammar-tripwire.sh /tmp/gt.bak
python3 - <<'PY'
import io
p='scripts/grammar-tripwire.sh'
s=io.open(p).read()
old='EXPECTED_SR=31'
new='EXPECTED_SR=999'
assert old in s, "baseline line not found"
io.open(p,'w').write(s.replace(old,new,1))
print("MUTATION APPLIED")
PY
grep -n '^EXPECTED_SR=' scripts/grammar-tripwire.sh
bazel test //tools:grammar_tripwire_test --nocache_test_results > /tmp/gtred.log 2>&1; echo "RED exit=$?"
grep -oE 'grammar-tripwire: FAIL[^"]*' /tmp/gtred.log | head -1
cp /tmp/gt.bak scripts/grammar-tripwire.sh
bazel test //tools:grammar_tripwire_test --nocache_test_results > /tmp/gtgreen.log 2>&1; echo "GREEN exit=$?"
git diff --stat scripts/grammar-tripwire.sh
```
Expected: the mutated line prints `EXPECTED_SR=999`, RED is non-zero and names the delta, GREEN is 0, and `git diff --stat` prints nothing.

- [ ] **Step 6: Commit**

```bash
git add tools/grammar_tripwire_test.sh tools/BUILD BUILD
git -c commit.gpgsign=false commit -m "test(bazel): gate the grammar conflict baseline

scripts/grammar-tripwire.sh asserts EXPECTED_SR=31 and EXPECTED_RR=0 exactly,
and CLAUDE.md makes any delta stop-the-line. The wrapper only locates the
script and the grammar in runfiles; the script already reads bison's status
without a pipe.

It runs bison itself rather than consuming //src/parser/gen, because the
baseline must be checked against the grammar as written and not against
whatever the genrule last produced.

Teeth proven by mutating the BASELINE rather than the grammar -- editing
parser.y risks leaving a real grammar change in the tree, and the comparison is
what is under test. With EXPECTED_SR=999 the test goes red and names the delta;
restored, it is green and git diff is empty.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: The lexer and AST libraries

**Files:**
- Create: `src/lexer/BUILD`, `src/ast/BUILD`

**Interfaces:**
- Consumes: `//tools:defs.bzl`.
- Produces: `//src/lexer` and `//src/ast`. Task 4 depends on both.

**Background.** `Makefile:81` and `Makefile:83` state these exactly:
`LEXER_SRCS = lexer/lexer.c lexer/token.c` and `AST_SRCS = ast/ast.c ast/ast_constructors.c`.

- [ ] **Step 1: Re-read the source lists rather than trusting this plan**

```bash
awk '/^LEXER_SRCS/{print}' Makefile | tr ' ' '\n' | grep -oE 'lexer/[a-z_]+\.c'
awk '/^AST_SRCS/{print}'   Makefile | tr ' ' '\n' | grep -oE 'ast/[a-z_]+\.c'
```
Expected: two files each. If either differs, use what the Makefile says.

- [ ] **Step 2: Create `src/lexer/BUILD`**

```python
load("//tools:defs.bzl", "goo_cc_library")

# Makefile:81 LEXER_SRCS.
goo_cc_library(
    name = "lexer",
    srcs = [
        "lexer.c",
        "token.c",
    ],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 3: Create `src/ast/BUILD`**

```python
load("//tools:defs.bzl", "goo_cc_library")

# Makefile:83 AST_SRCS.
goo_cc_library(
    name = "ast",
    srcs = [
        "ast.c",
        "ast_constructors.c",
    ],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 4: Build both, and resolve any missing private header the way phase 1 did**

```bash
bazel build //src/lexer //src/ast > /tmp/la.log 2>&1; echo "EXIT=$?"
grep -E 'fatal error|ERROR' /tmp/la.log | head -5
```
Expected: exit 0. A `fatal error: X.h: No such file or directory` means a private sibling header is undeclared — the same class as `src/runtime/platform.h` in phase 1. Add it to that target's `srcs`, not to `//include:headers`.

- [ ] **Step 5: Confirm neither reaches LLVM, with a control**

```bash
bazel query 'somepath(//src/lexer + //src/ast, @llvm//:llvm_c)' --noshow_progress 2>/dev/null | wc -l
bazel query 'somepath(//third_party/llvm:llvm_smoke, @llvm//:llvm_c)' --noshow_progress 2>/dev/null | wc -l
```
Expected: `0` then `2`. The second is the positive control — without it, the `0` proves nothing.

- [ ] **Step 6: Commit**

```bash
git add src/lexer/BUILD src/ast/BUILD
git -c commit.gpgsign=false commit -m "build(bazel): the lexer and AST libraries

Makefile:81 LEXER_SRCS and Makefile:83 AST_SRCS, verbatim. Neither reaches
@llvm//:llvm_c, confirmed by bazel query somepath returning empty against a
positive control that returns a two-node path.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: The parser library

**Files:**
- Modify: `src/parser/BUILD`

**Interfaces:**
- Consumes: `//src/parser/gen:parser_tab` and `:parser_tab_c` from Task 1; `//src/lexer` and `//src/ast` from Task 3.
- Produces: `//src/parser`, exposing `parse_input()` and `ast_root`. Task 6 depends on it.

**Background.** `Makefile:82`: `PARSER_SRCS = parser/parser.tab.c parser/lexer_bridge.c parser/parser_errors.c parser/parser_actions.c`. The first of those is generated, so this target takes it from `//src/parser/gen` and compiles the other three from this package.

- [ ] **Step 1: Confirm which sources spell `#include "parser.tab.h"`**

```bash
grep -rn '#\s*include.*parser\.tab\.h' src/parser/
```
Expected: `lexer_bridge.c`, `parser_actions.c`, and the generated `parser.tab.c` itself. All three need `//src/parser/gen:parser_tab` on the include path.

- [ ] **Step 2: Append the parser library to `src/parser/BUILD`**

```python
load("//tools:defs.bzl", "goo_cc_library")

# Makefile:82 PARSER_SRCS. parser.tab.c is NOT listed as a local source: it
# comes from //src/parser/gen, which runs bison. The on-disk
# src/parser/parser.tab.c that Make leaves behind is gitignored and is
# deliberately not referenced by anything here.
goo_cc_library(
    name = "parser",
    srcs = [
        "lexer_bridge.c",
        "parser_actions.c",
        "parser_errors.c",
        "//src/parser/gen:parser_tab_c",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//src/ast",
        "//src/lexer",
        "//src/parser/gen:parser_tab",
    ],
)
```

- [ ] **Step 3: Build, and run the discovery loop on any link or compile failure**

```bash
bazel build //src/parser > /tmp/p.log 2>&1; echo "EXIT=$?"
grep -E 'ERROR|fatal error' /tmp/p.log | head -5
```
Read the error text before pattern-matching it. For an `undefined symbol: X` (this toolchain uses `ld.lld`, not GNU ld), find the definer:
```bash
grep -rln '^[a-zA-Z_].*\bX\s*(' src/ --include='*.c'
```
and add the owning library to `deps`. Stop and report if the loop reaches `src/codegen/` — that means the parser needs LLVM, which contradicts the measurement in the spec and is worth understanding before encoding.

- [ ] **Step 4: Prove the build used the GENERATED parser, not Make's leftover**

This is the claim the whole package split exists to protect:
```bash
bazel aquery 'mnemonic("CppCompile", //src/parser)' --output=text 2>/dev/null \
  | grep -oE '[a-z/_-]*parser\.tab\.c' | sort -u
```
Expected: a path under `bazel-out/.../src/parser/gen/`, and NOT a bare `src/parser/parser.tab.c`.

- [ ] **Step 5: Prove it is genuinely regenerated from the grammar**

```bash
bazel build //src/parser > /dev/null 2>&1
cp src/parser/parser.y /tmp/parser.y.bak
printf '\n/* bazel input probe -- reverted immediately */\n' >> src/parser/parser.y
bazel aquery 'mnemonic("Genrule", //src/parser/gen:bison)' --output=text > /tmp/aq1.log 2>&1
bazel build //src/parser > /tmp/rebuild.log 2>&1; echo "rebuild exit=$?"
cp /tmp/parser.y.bak src/parser/parser.y
git diff --stat src/parser/parser.y
```
Expected: the rebuild succeeds, and `git diff --stat` prints nothing afterwards. Do **not** test this with `touch` — Bazel digests content, not mtime, and phase 0 wasted a check on exactly that.

- [ ] **Step 6: Confirm no LLVM, with a control**

```bash
bazel query 'somepath(//src/parser, @llvm//:llvm_c)' --noshow_progress 2>/dev/null | wc -l
bazel query 'somepath(//third_party/llvm:llvm_smoke, @llvm//:llvm_c)' --noshow_progress 2>/dev/null | wc -l
```
Expected: `0` then `2`.

- [ ] **Step 7: Commit**

```bash
git add src/parser/BUILD
git -c commit.gpgsign=false commit -m "build(bazel): the parser library, built from the generated grammar

Makefile:82 PARSER_SRCS, with one deliberate difference: parser.tab.c comes
from //src/parser/gen rather than from the working tree. Make leaves a
gitignored src/parser/parser.tab.c on disk and nothing here references it.

Verified by aquery that the CppCompile action reads the bazel-out copy under
src/parser/gen and not the on-disk one, which is the claim the package split
exists to protect.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: The type checker and its neighbours

**Files:**
- Create: `src/errors/BUILD`, `src/comptime/BUILD`, `src/package/BUILD`
- Modify: `src/types/BUILD`

**Interfaces:**
- Consumes: `//src/ast`, `//src/parser`.
- Produces: `//src/types:frontend`, exposing `type_checker_new()`, `type_check_program()` and `type_checker_free()`. Task 6 depends on it.

**Background.** `Makefile:119` `GOO_TYPES_SRCS` names 16 files. Phase 1 already built five of them as separate libraries; this task adds the target that carries the whole front-end type checker, keeping those five as their own libraries so the fine-grained boundaries survive.

- [ ] **Step 1: Read the real list**

```bash
awk '/^GOO_TYPES_SRCS/{print}' Makefile | tr ' ' '\n' | grep -oE 'types/[a-z_]+\.c' | sed 's|types/||' | sort
```
Expected: 16 files. Use this output, not the list below, if they differ.

- [ ] **Step 2: Add the neighbouring libraries**

`src/errors/BUILD` (Makefile:87 `ERROR_SRCS`):
```python
load("//tools:defs.bzl", "goo_cc_library")

goo_cc_library(
    name = "errors",
    srcs = ["error.c"],
    visibility = ["//visibility:public"],
)
```

`src/comptime/BUILD` (Makefile:97 `COMPTIME_SRCS`):
```python
load("//tools:defs.bzl", "goo_cc_library")

goo_cc_library(
    name = "comptime",
    srcs = [
        "comptime.c",
        "comptime_intrinsics.c",
        "comptime_types.c",
        "comptime_value.c",
    ],
    visibility = ["//visibility:public"],
    deps = ["//src/ast"],
)
```

`src/package/BUILD` (Makefile:94 `PACKAGE_SRCS`):
```python
load("//tools:defs.bzl", "goo_cc_library")

goo_cc_library(
    name = "package",
    srcs = ["import_resolver.c"],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 3: Append the front-end library to `src/types/BUILD`**

```python
# The full front-end type checker: Makefile:119 GOO_TYPES_SRCS, minus the five
# units already declared above as their own libraries, which are picked up as
# deps instead. Splitting them out is deliberate -- it is the fine-grained
# boundary the Makefile's single $(SRC_OBJS) link hides.
#
# No LLVM. Every LLVM identifier in type_checker.c, expression_checker.c and
# shim_signatures.c is inside a // comment; the includes are types.h,
# comptime.h, embedding.h and lane_ownership.h.
goo_cc_library(
    name = "frontend",
    srcs = [
        "embedding.c",
        "expression_checker.c",
        "expression_helpers.c",
        "lane_ownership.c",
        "nonretaining.c",
        "ownership_checker.c",
        "shim_signatures.c",
        "tc_fctx.c",
        "terminating_stmt.c",
        "type_checker.c",
        "types.c",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":block_escape",
        ":escape_core",
        ":local_escape",
        ":param_escape",
        ":release_decision",
        "//src/ast",
        "//src/comptime",
        "//src/errors",
        "//src/package",
    ],
)
```

- [ ] **Step 4: Run the discovery loop**

```bash
bazel build //src/types:frontend > /tmp/fe.log 2>&1; echo "EXIT=$?"
grep -E 'ERROR|fatal error' /tmp/fe.log | head -8
```
Resolve each failure as in Task 4. Two stop conditions, both meaning "report rather than encode":
- the loop reaches `src/codegen/`, contradicting the spec's LLVM boundary;
- it exceeds eight iterations, meaning the front end is not separable and the coupling deserves discussion first.

- [ ] **Step 5: Confirm no LLVM, with a control**

```bash
bazel query 'somepath(//src/types:frontend, @llvm//:llvm_c)' --noshow_progress 2>/dev/null | wc -l
bazel query 'somepath(//third_party/llvm:llvm_smoke, @llvm//:llvm_c)' --noshow_progress 2>/dev/null | wc -l
```
Expected: `0` then `2`. A non-zero first number falsifies the spec's claim that LLVM reaches only 15 files — stop and re-measure rather than adding the dependency.

- [ ] **Step 6: Commit**

```bash
git add src/errors/BUILD src/comptime/BUILD src/package/BUILD src/types/BUILD
git -c commit.gpgsign=false commit -m "build(bazel): the front-end type checker and its neighbours

Makefile:119 GOO_TYPES_SRCS, minus the five analysis units phase 1 already
declared as their own libraries, which become deps instead. Keeping them split
is the point: it is the fine-grained boundary that linking \$(SRC_OBJS) hides.

No LLVM, confirmed by bazel query somepath against a positive control. Every
LLVM identifier in type_checker.c, expression_checker.c and shim_signatures.c
is inside a comment.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 6: Port release_decision_test

**Files:**
- Create: `tests/unit/types/BUILD`

**Interfaces:**
- Consumes: `//src/parser`, `//src/types:frontend`, `//tests/unit:goo_check`.
- Produces: `//tests/unit/types:release_decision_test`, claiming the `release-decision-test` gate.

**Background.** Phase 1 attempted this and stopped at the documented stop condition: linking against `release_decision.c` alone gave `undefined symbol: parse_input, ast_root, type_checker_new, type_check_program, type_checker_free`. Those five now have homes.

- [ ] **Step 1: Write the test target**

```python
load("//tools:defs.bzl", "goo_cc_test")

# Phase 1 could not build these: they #include "parser.h" and call
# parse_input() and type_check(), so they parse real Goo source and type-check
# it before asserting on the analysis. They are integration tests of the front
# end wearing the shape of unit tests, and linking $(SRC_OBJS) for all of them
# is what kept that hidden.
goo_cc_test(
    name = "release_decision_test",
    size = "small",
    srcs = ["release_decision_test.c"],
    deps = [
        "//src/parser",
        "//src/types:frontend",
        "//src/types:release_decision",
        "//tests/unit:goo_check",
    ],
)
```

- [ ] **Step 2: Run it, and read the error before matching on it**

```bash
bazel test //tests/unit/types:release_decision_test --test_output=errors > /tmp/rd.log 2>&1; echo "EXIT=$?"
grep -E 'ERROR|undefined symbol|PASSED|FAILED' /tmp/rd.log | head -8
```
Resolve any `undefined symbol:` by finding the definer and adding its library, as in Task 4.

- [ ] **Step 3: Confirm the two build systems agree on the VERDICT, not the exit code**

```bash
make release-decision-test > /tmp/mk_rd.log 2>&1; echo "make exit=$?"
bazel test //tests/unit/types:release_decision_test --test_output=all --nocache_test_results > /tmp/bz_rd.log 2>&1; echo "bazel exit=$?"
grep -E 'release-decision-test:|checks,.*rows' /tmp/mk_rd.log | tail -1
grep -E 'release-decision-test:|checks,.*rows' /tmp/bz_rd.log | tail -1
```
Expected: both exit 0 **and** both print the same check/row counts. Phase 1 established this as the standard of agreement: matching exit codes alone would not have caught a suite whose table stopped executing.

- [ ] **Step 4: Commit**

```bash
git add tests/unit/types/BUILD
git -c commit.gpgsign=false commit -m "build(bazel): port release_decision_test

Phase 1 stopped here at its documented stop condition: linking against
release_decision.c alone gave undefined parse_input, ast_root, type_checker_new,
type_check_program and type_checker_free. All five now have homes.

Agreement checked on the verdict rather than the exit code: both systems report
the same check and row counts.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 7: Port the three escape suites

**Files:**
- Modify: `tests/unit/types/BUILD`

**Interfaces:**
- Consumes: everything from Task 6.
- Produces: `param_escape_test`, `block_escape_test`, `local_escape_test`.

- [ ] **Step 1: Append the three targets**

```python
goo_cc_test(
    name = "param_escape_test",
    size = "small",
    srcs = ["param_escape_test.c"],
    deps = [
        "//src/parser",
        "//src/types:frontend",
        "//src/types:param_escape",
        "//tests/unit:goo_check",
    ],
)

# block_escape_test.c includes param_escape.h as well as block_escape.h.
goo_cc_test(
    name = "block_escape_test",
    size = "small",
    srcs = ["block_escape_test.c"],
    deps = [
        "//src/parser",
        "//src/types:block_escape",
        "//src/types:frontend",
        "//src/types:param_escape",
        "//tests/unit:goo_check",
    ],
)

goo_cc_test(
    name = "local_escape_test",
    size = "small",
    srcs = ["local_escape_test.c"],
    deps = [
        "//src/parser",
        "//src/types:frontend",
        "//src/types:local_escape",
        "//tests/unit:goo_check",
    ],
)
```

- [ ] **Step 2: Run all four and resolve failures**

```bash
bazel test //tests/unit/types:all --test_output=errors > /tmp/four.log 2>&1; echo "EXIT=$?"
grep -E 'undefined symbol|PASSED|FAILED' /tmp/four.log | head -12
```

- [ ] **Step 3: Compare verdicts against Make for all four**

```bash
for t in release-decision param-escape block-escape local-escape; do
  make "${t}-test" > "/tmp/mk_${t}.log" 2>&1; m=$?
  b=$(echo "$t" | tr '-' '_')
  bazel test "//tests/unit/types:${b}_test" --test_output=all --nocache_test_results > "/tmp/bz_${t}.log" 2>&1; z=$?
  printf '%-18s make=%s bazel=%s\n' "$t" "$m" "$z"
  echo "   make : $(grep -oE '[0-9]+ checks, [0-9]+ rows' "/tmp/mk_${t}.log" | tail -1)"
  echo "   bazel: $(grep -oE '[0-9]+ checks, [0-9]+ rows' "/tmp/bz_${t}.log" | tail -1)"
done
```
Expected: every status 0, and each pair of check/row counts identical. A mismatch means the Bazel dependency set differs from what Make links, and the Bazel one is the suspect.

- [ ] **Step 4: Commit**

```bash
git add tests/unit/types/BUILD
git -c commit.gpgsign=false commit -m "build(bazel): port the three escape suites

Same shape as release_decision_test: each needs the parser and the front-end
type checker because it parses and type-checks real Goo source before
asserting. block_escape_test also includes param_escape.h, so it takes both.

All four verdicts compared against Make on check and row counts, not exit
codes.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 8: Prove all four ported suites kept their teeth

**Files:** none. This task runs mutations and records the results.

**Interfaces:**
- Consumes: the four targets from Tasks 6 and 7.
- Produces: evidence, in the commit message, that each reports RED on a real defect.

**Background.** `scripts/release_decision_teeth.sh` already mutates `release_decision.c` one condition at a time and asserts a row goes red, and `scripts/escape_teeth.sh` does the same for the escape reasons. Read them for predicates known to be covered rather than guessing.

- [ ] **Step 1: Read the existing mutation harnesses for covered predicates**

```bash
head -40 scripts/release_decision_teeth.sh
head -40 scripts/escape_teeth.sh
```
Record one predicate per suite that the harness already proves is covered.

- [ ] **Step 2: Mutate release_decision, confirm RED, restore, confirm GREEN**

Worked example, using `cond3-arena` -- a condition `scripts/release_decision_teeth.sh:84` already proves is covered. Write the mutation to a file rather than a heredoc nested inside another heredoc:

```bash
cp src/types/release_decision.c /tmp/mutant.bak
cat > /tmp/mutate.py <<'EOF'
import io
p = 'src/types/release_decision.c'
s = io.open(p, encoding='utf-8').read()
old = '    if (r->arena_depth > 0) return RELEASE_NO_ARENA;'
new = '    if (false) return RELEASE_NO_ARENA;'
assert old in s, "target predicate not found -- do not proceed"
io.open(p, 'w', encoding='utf-8').write(s.replace(old, new, 1))
print("MUTATION APPLIED")
EOF
python3 /tmp/mutate.py
diff /tmp/mutant.bak src/types/release_decision.c | head -4
bazel test //tests/unit/types:release_decision_test --nocache_test_results > /tmp/red.log 2>&1
echo "RED exit=$?"
cp /tmp/mutant.bak src/types/release_decision.c
bazel test //tests/unit/types:release_decision_test --nocache_test_results > /tmp/green.log 2>&1
echo "GREEN exit=$?"
git diff --stat src/types/release_decision.c
```

Expected: `MUTATION APPLIED`, a two-line diff, RED non-zero, GREEN 0, and an empty `git diff --stat`.

Use python with the `assert`, never `sed`. A phase 1 `sed` silently did nothing because an unescaped pipe character collided with its `s|||` delimiter, and the resulting PASS read as a missing tooth. The `assert` plus the `diff` make a no-op mutation impossible to mistake for a green result.

- [ ] **Step 3: Repeat for the three escape suites**

Get each one's covered predicate from the harness that already proves it, rather than choosing one:

```bash
grep -nE '^"(cond|loop|alias|values)' scripts/release_decision_teeth.sh
grep -nE '^"[a-z-]+' scripts/escape_teeth.sh | head -20
```

Each line is `name`, tab, `original`, optionally tab and `replacement`. Take the `original` field as the `old` string and apply the same procedure as Step 2, against:

| Suite | Unit to mutate |
|---|---|
| `param_escape_test` | `src/types/param_escape.c` |
| `block_escape_test` | `src/types/block_escape.c` |
| `local_escape_test` | `src/types/local_escape.c` |

Expected per suite: RED non-zero, GREEN 0, `git diff --stat` empty. If a suite stays green under three different covered predicates, stop and report -- its Bazel dependency set probably omits the unit under test.

- [ ] **Step 4: Confirm the tree is clean before committing**

```bash
git status --short src/ include/ scripts/
```
Expected: no output.

- [ ] **Step 5: Commit the evidence**

```bash
git -c commit.gpgsign=false commit --allow-empty -m "test(bazel): record that all four ported suites kept their teeth

A ported test that passes proves the build works, not that the test still
tests anything. Each of the four suites was mutated on a predicate that
scripts/release_decision_teeth.sh or scripts/escape_teeth.sh already proves is
covered, confirmed RED, restored and confirmed GREEN, with a clean tree after.

Mutations are applied with python and an assert, not sed: a phase 1 sed
silently did nothing when an unescaped | collided with its delimiter, and the
resulting PASS read as a missing tooth.

<per-suite: file:line mutated, RED exit, GREEN exit>

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```
Replace the placeholder line with the real per-suite results.

---

### Task 9: Close phase 2

**Files:**
- Modify: `docs/superpowers/specs/2026-08-25-bazel-migration-design.md` if any measurement in it turned out wrong.

- [ ] **Step 1: Read the parity count**

```bash
./tools/parity.sh > /tmp/p2.log 2>&1; echo "exit=$?"; head -3 /tmp/p2.log
```
Expected: `unmapped` has fallen from 215 by the number of gates newly claimed — four suites, plus `grammar-tripwire` if Task 2 Step 1 found it in `VERIFY_ALL_DEPS`.

- [ ] **Step 2: Confirm each newly claimed gate is actually mapped**

```bash
for g in release-decision-test param-escape-test block-escape-test local-escape-test grammar-tripwire; do
  printf '%-24s %s\n' "$g" "$(grep -cx "  $g" /tmp/p2.log)"
done
```
Expected: `0` for each of the four suites, meaning none is still listed as unmapped. A `1` means the Bazel target name does not match the convention — fix the target name, never `parity.sh`.

- [ ] **Step 3: Confirm both build systems are green, and the self-test still has teeth**

```bash
bazel test //... > /tmp/bz2.log 2>&1; echo "bazel exit=$?"
grep -cE '^//.*PASSED' /tmp/bz2.log
./tools/parity_selftest.sh; echo "selftest exit=$?"
make verify-core > /tmp/vc3.log 2>&1; echo "verify-core exit=$?"
tail -2 /tmp/vc3.log
```
Run `make verify-core` backgrounded with a 600s window. Expected: all three exit 0.

- [ ] **Step 4: Confirm the grammar baseline did not move**

Nothing in this phase edits `parser.y`, so this must be exactly the starting number:
```bash
bash scripts/grammar-tripwire.sh src/parser/parser.y
```
Expected: `grammar-tripwire: PASS (31 S/R + 0 R/R — baseline exact)`.

- [ ] **Step 5: Update the gate-count baseline only if a gate was added or removed**

```bash
./tools/parity.sh --list-make-gates | grep -c .
cat tools/parity-gate-count.txt
```
Expected: identical. This phase adds no Makefile gate, so a difference means something unintended changed the Makefile.

- [ ] **Step 6: Push and open the phase 2 PR**

```bash
git push -u origin build/bazel-phase2-frontend
gh pr create --title "build(bazel): phase 2, the LLVM-free front end" --body "$(cat <<'BODY'
Phase 2 of the Bazel migration. Spec: `docs/superpowers/specs/2026-08-25-bazel-migration-design.md`. Plan: `docs/superpowers/plans/2026-08-25-bazel-phase2-frontend.md`.

Still additive. `make verify-core` is the gate and is green; no Makefile target is removed.

## What is here

- `//src/parser/gen` runs bison as a **two-output genrule**, replacing the GNU Make 4.3 grouped-target workaround. Confirmed by `aquery` to be exactly one action.
- The grammar conflict tripwire is a Bazel test, asserting `31 S/R + 0 R/R` exactly.
- `//src/lexer`, `//src/ast`, `//src/parser`, `//src/errors`, `//src/comptime`, `//src/package`, `//src/types:frontend`.
- The four `tests/unit/types` suites phase 1 could not build.

## Why the generated parser lives in its own package

`src/parser/parser.tab.c` is gitignored but Make writes it to disk. A genrule declaring that name inside `//src/parser` would collide with a real file **on developer machines and not on a clean checkout** — a build that behaves differently depending on whether `make` has run. `aquery` confirms the compile reads the `bazel-out` copy.

## Correcting phase 1

Phase 1 sent these four suites to phase 3, citing "7 LLVM references" in `src/types/type_checker.c`. **Every one of those is inside a `//` comment.** The claim came from a grep count that was never opened. LLVM enters this tree through `include/codegen.h` and `include/codegen_cfctx.h` only, reaching the 14 files in `src/codegen/` plus `src/compiler/goo.c`. The suites needed the parser, nothing more.

## Parity

215 → <N>. Still red until phase 7.

## Teeth

The tripwire and all four suites are proven able to fail: each mutated on a predicate an existing harness already covers, confirmed RED, restored, confirmed GREEN, clean tree. Mutations use python with an `assert`, not `sed`.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01DHrKLCPRvkeBRutFyP2vsh
BODY
)"
```
Replace `<N>` with the real number from Step 1.

- [ ] **Step 7: Read the real CI conclusion**

```bash
gh pr view <PR#> --json statusCheckRollup --jq '.statusCheckRollup[] | "\(.conclusion // .state)\t\(.name // .context)"'
```
Never a piped exit code. All four checks must read SUCCESS.

---

## What phase 3 now needs

Phase 3 shrinks to what actually depends on LLVM: the 14 files in `src/codegen/` plus `src/compiler/goo.c`, producing `//src/compiler:goo`, compared against `make bin/goo` on the same golden suite. `include/value_scope.h` includes `codegen.h`, so `src/codegen/value_scope.c` belongs there too despite having no LLVM identifier of its own.

The open questions this phase will answer, which the phase 3 plan needs:

- Does `//src/types:frontend` link without any `src/codegen` symbol? If the discovery loop in Task 5 pulls one in, the LLVM boundary is not where the spec says it is, and phase 3's scope grows.
- How many gates does one ported suite claim? Task 9 Step 1 measures it, which is what makes the phase 4 estimate for the 155 inline probes real rather than assumed.
- Does the bison genrule's output differ byte-for-byte from Make's? If it does, phase 3's `goo`-versus-`goo` comparison has a second variable in it and must control for that first.
