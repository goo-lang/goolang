# Phase 0: Program Dump, Differential Harness, Diagnostic Catalogue — Implementation Plan

> **Superseded path:** This plan names `src/ast/program_dump.c` throughout. The file moved to `src/compiler/program_dump.c` in Step 3. Read the code at that path, not at the path this plan gives.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `bin/goo` a deterministic, versioned JSON dump of the parsed and the typed program (with release plan), a harness that diffs two producers of that dump across every golden fixture, and a machine-readable catalogue of the type checker's diagnostic strings — the three things every later front-end migration step is measured against.

**Architecture:** A tiny streaming JSON writer (`src/ast/json_writer.c`) feeds a walker (`src/ast/program_dump.c`) that emits one object per AST node, a type table by id, and one plan section per file. Two driver flags select the stage: `--emit-ast-json` after parsing, `--emit-program` after `type_check_program_files` plus `release_plan_analyze`. Every unhandled node or type kind aborts by name, so the fixture gate is the coverage proof. Nothing in codegen changes.

**Tech Stack:** C23 (existing toolchain), bash probes following `scripts/*_probe.sh` conventions (every probe has `--self-test` teeth), python3 (already in the CI image) for the structural checker and the diagnostic extractor. No new libraries: the hand-written writer replaces reaching for json-c so key order and number formatting are ours.

**Parent plan:** `docs/superpowers/plans/2026-09-04-haskell-zig-strangler-plan.md`, tasks 2–5. Task 1 (toolchain pin) is deferred to the plan that first uses GHC.

**Environment note:** the checkout does not build on macOS (see `.claude` memory `goolang-macos-portability`). Run every build/test step inside the Linux container: `scratchpad/ci.sh '<make target>'` syncs the checkout into the `goo-work` volume and runs the command there. All "Run:" lines below assume that wrapper or a Linux box.

---

## File structure

| File | Responsibility |
|---|---|
| `include/json_writer.h`, `src/ast/json_writer.c` | Streaming JSON writer. Keys in call order, 2-space indent, escapes, embedded-NUL-safe strings. No knowledge of the AST. |
| `include/program_dump.h`, `src/ast/program_dump.c` | Walks `ASTNode` trees and emits the dump. Owns the type table (pointer→id) and the per-kind field emitters. Aborts on unknown kinds. |
| `src/compiler/goo.c` | Two flags, two call sites. |
| `tests/unit/ast/json_writer_test.c` | Unit rows for the writer (exact-output). |
| `scripts/program_dump_probe.sh` | Gate: dump every fixture at both stages, determinism, structural check. `--self-test`. |
| `scripts/program_dump_check.py` | Structural validator for a dump file. |
| `scripts/frontend_diff.sh` | Gate harness: two producers, per-fixture diff, counts. `--self-test`. |
| `scripts/extract_diagnostics.py` | Regenerates `catalogue/diagnostics.tsv` from `src/types/*.c`. |
| `catalogue/diagnostics.tsv` | Committed catalogue. |
| `scripts/diagnostics_drift_probe.sh` | Gate: regenerate and diff. `--self-test`. |
| `Makefile` | `AST_SRCS` additions, four new targets, `VERIFY_ALL_DEPS` entries. |
| `docs/superpowers/specs/2026-09-04-program-dump-format.md` | The format, one section per node family. Written from the emitters, after they exist. |

---

## Dump format (reference for every task)

```json
{
  "goo_program_dump": 1,
  "stage": "parse",
  "files": [
    {
      "file": "examples/erru_catch_probe.goo",
      "package": "main",
      "imports": [ {"kind": "IMPORT_SPEC", "pos": [5, 8, 61], "path": "fmt", "alias": null} ],
      "decls": [ { "kind": "FUNC_DECL", "pos": [7, 1, 74], "type": 3, "name": "alwaysOk", ... } ],
      "plan": null
    }
  ],
  "types": [ {"id": 0, "kind": "INT64", "name": "int", "size": 8, "align": 8} ]
}
```

- `pos` is always `[line, column, offset]`; the filename is at file level.
- `"type": <id>` appears on any node whose `node_type` is non-NULL. In the `parse` stage no node has one, and `types` is `[]`.
- A `next`-chained list is an array. A single child that is NULL is `null`.
- Enumerations (token operators, ownership, channel pattern, pattern type) are emitted as their C identifier names (`"PLUS"`, `"OWNERSHIP_OWNED"`).
- `types[i].id == i`. Ids are assigned in first-visit order during the walk, and component types are appended while the table is being emitted, so the table is closed under reference.
- The `plan` value is `null` in the parse stage, or when `GOO_ARC_RELEASE=0`; otherwise an array with one object per function.

---

### Task 1: JSON writer

**Files:**
- Create: `include/json_writer.h`
- Create: `src/ast/json_writer.c`
- Create: `tests/unit/ast/json_writer_test.c`
- Modify: `Makefile:83` (`AST_SRCS`), and add a test target next to `obj_header_test` (`Makefile:5061`)

- [ ] **Step 1: Write the failing test**

`tests/unit/ast/json_writer_test.c`:

```c
// json_writer_test: the dump's writer must be byte-exact and deterministic.
// Rows are exact-string comparisons against a memstream, because the whole
// point of the writer is that two runs over the same tree produce the same
// bytes — a "looks like JSON" check would pass a writer that reorders keys.
#include "../goo_check.h"
#include "json_writer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define JW_ROWS 6

static char* capture(void (*fn)(JsonW*)) {
    char* buf = NULL; size_t n = 0;
    FILE* f = open_memstream(&buf, &n);
    JsonW w; jw_init(&w, f);
    fn(&w);
    fclose(f);
    return buf;
}

static void row1(JsonW* w) { jw_begin_object(w); jw_end_object(w); }
static void row2(JsonW* w) {
    jw_begin_object(w);
    jw_key(w, "a"); jw_int(w, 1);
    jw_key(w, "b"); jw_string(w, "x");
    jw_end_object(w);
}
static void row3(JsonW* w) {
    jw_begin_array(w); jw_int(w, -1); jw_bool(w, true); jw_null(w); jw_end_array(w);
}
static void row4(JsonW* w) { jw_string(w, "q\"b\\s\n\t\x01"); }
static void row5(JsonW* w) { jw_string_len(w, "a\0b", 3); }
static void row6(JsonW* w) {
    jw_begin_object(w);
    jw_key(w, "o"); jw_begin_object(w); jw_key(w, "k"); jw_begin_array(w); jw_end_array(w); jw_end_object(w);
    jw_end_object(w);
}

int main(void) {
    goo_check_expect(JW_ROWS);
    char* s;

    goo_check_row(1, "empty object");
    s = capture(row1); goo_check(strcmp(s, "{}") == 0, s); free(s);

    goo_check_row(2, "keys in call order, 2-space indent");
    s = capture(row2); goo_check(strcmp(s, "{\n  \"a\": 1,\n  \"b\": \"x\"\n}") == 0, s); free(s);

    goo_check_row(3, "array of scalars");
    s = capture(row3); goo_check(strcmp(s, "[\n  -1,\n  true,\n  null\n]") == 0, s); free(s);

    goo_check_row(4, "escapes: quote, backslash, newline, tab, control");
    s = capture(row4); goo_check(strcmp(s, "\"q\\\"b\\\\s\\n\\t\\u0001\"") == 0, s); free(s);

    goo_check_row(5, "embedded NUL survives as \\u0000");
    s = capture(row5); goo_check(strcmp(s, "\"a\\u0000b\"") == 0, s); free(s);

    goo_check_row(6, "nested empty containers stay on one line");
    s = capture(row6); goo_check(strcmp(s, "{\n  \"o\": {\n    \"k\": []\n  }\n}") == 0, s); free(s);

    return goo_check_done("json_writer_test");
}
```

- [ ] **Step 2: Add the Makefile target and run the test to see it fail to build**

Append to `Makefile` directly after the `obj-header-test` stanza (line 5066):

```make
# Phase 0 (program dump): the writer is byte-exact by contract. Rows compare
# whole strings, so a key reorder or an indent change is a red row, not a
# style nit.
json_writer_test: $(TEST_UNIT_DIR)/ast/json_writer_test.c $(TEST_UNIT_DIR)/goo_check.h $(SRCDIR)/ast/json_writer.c
	$(CC) $(CFLAGS) -o $@ $< $(SRCDIR)/ast/json_writer.c

json-writer-test: json_writer_test
	@echo "Running JSON writer tests..."
	./json_writer_test
```

Run: `make json-writer-test`
Expected: compile error, `json_writer.h: No such file or directory`.

- [ ] **Step 3: Write the header**

`include/json_writer.h`:

```c
#ifndef JSON_WRITER_H
#define JSON_WRITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

// Streaming JSON writer for the program dump. Deterministic by construction:
// keys are written in call order, numbers are printed with fixed formats,
// and there is no buffering or sorting that could reorder output between
// runs. That property is what lets two producers of the same tree be
// compared with `diff`.
//
// Nesting deeper than JW_MAX_DEPTH aborts rather than corrupting the output:
// a truncated dump that still parses would defeat the differential gates.
#define JW_MAX_DEPTH 512

typedef struct {
    FILE* out;
    int   depth;
    bool  first[JW_MAX_DEPTH];   // per open container: no element written yet
    bool  in_object[JW_MAX_DEPTH];
    bool  after_key;             // a key was written; the value follows inline
} JsonW;

void jw_init(JsonW* w, FILE* out);
void jw_begin_object(JsonW* w);
void jw_end_object(JsonW* w);
void jw_begin_array(JsonW* w);
void jw_end_array(JsonW* w);
void jw_key(JsonW* w, const char* key);
void jw_string(JsonW* w, const char* s);              // NULL -> null
void jw_string_len(JsonW* w, const char* s, size_t n); // embedded NUL safe
void jw_int(JsonW* w, long long v);
void jw_uint(JsonW* w, unsigned long long v);
void jw_bool(JsonW* w, bool v);
void jw_null(JsonW* w);

#endif
```

- [ ] **Step 4: Write the implementation**

`src/ast/json_writer.c`:

```c
#include "json_writer.h"
#include <stdlib.h>
#include <string.h>

static void die(const char* what) {
    fprintf(stderr, "json_writer: %s\n", what);
    abort();
}

void jw_init(JsonW* w, FILE* out) {
    memset(w, 0, sizeof(*w));
    w->out = out;
}

static void indent(JsonW* w) {
    for (int i = 0; i < w->depth; i++) fputs("  ", w->out);
}

// Called before any value or key: writes the separator/newline/indent that
// positions the next element. A value directly after a key stays inline.
static void before_element(JsonW* w) {
    if (w->after_key) { w->after_key = false; return; }
    if (w->depth == 0) return;
    if (!w->first[w->depth - 1]) fputc(',', w->out);
    w->first[w->depth - 1] = false;
    fputc('\n', w->out);
    indent(w);
}

static void open_container(JsonW* w, char c, bool is_object) {
    before_element(w);
    if (w->depth >= JW_MAX_DEPTH) die("nesting exceeds JW_MAX_DEPTH");
    fputc(c, w->out);
    w->first[w->depth] = true;
    w->in_object[w->depth] = is_object;
    w->depth++;
}

static void close_container(JsonW* w, char c) {
    if (w->depth == 0) die("close with no open container");
    w->depth--;
    if (!w->first[w->depth]) { fputc('\n', w->out); indent(w); }
    fputc(c, w->out);
}

void jw_begin_object(JsonW* w) { open_container(w, '{', true); }
void jw_end_object(JsonW* w)   { close_container(w, '}'); }
void jw_begin_array(JsonW* w)  { open_container(w, '[', false); }
void jw_end_array(JsonW* w)    { close_container(w, ']'); }

static void write_escaped(JsonW* w, const char* s, size_t n) {
    fputc('"', w->out);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  fputs("\\\"", w->out); break;
            case '\\': fputs("\\\\", w->out); break;
            case '\n': fputs("\\n", w->out); break;
            case '\t': fputs("\\t", w->out); break;
            case '\r': fputs("\\r", w->out); break;
            default:
                if (c < 0x20) fprintf(w->out, "\\u%04x", c);
                else fputc(c, w->out);   // UTF-8 bytes pass through verbatim
        }
    }
    fputc('"', w->out);
}

void jw_key(JsonW* w, const char* key) {
    if (w->depth == 0 || !w->in_object[w->depth - 1]) die("key outside an object");
    before_element(w);
    write_escaped(w, key, strlen(key));
    fputs(": ", w->out);
    w->after_key = true;
}

void jw_string_len(JsonW* w, const char* s, size_t n) {
    before_element(w);
    if (!s) { fputs("null", w->out); return; }
    write_escaped(w, s, n);
}

void jw_string(JsonW* w, const char* s) { jw_string_len(w, s, s ? strlen(s) : 0); }

void jw_int(JsonW* w, long long v)           { before_element(w); fprintf(w->out, "%lld", v); }
void jw_uint(JsonW* w, unsigned long long v) { before_element(w); fprintf(w->out, "%llu", v); }
void jw_bool(JsonW* w, bool v)               { before_element(w); fputs(v ? "true" : "false", w->out); }
void jw_null(JsonW* w)                       { before_element(w); fputs("null", w->out); }
```

- [ ] **Step 5: Run the test**

Run: `make json-writer-test`
Expected: `json_writer_test: 6 rows, 6 checks, 0 failures` (the exact summary line format comes from `goo_check_done`; the important part is `0 failures` and exit 0).

- [ ] **Step 6: Add the sources to the compiler build and the gate list**

`Makefile:83`:
```make
AST_SRCS = $(SRCDIR)/ast/ast.c $(SRCDIR)/ast/ast_constructors.c $(SRCDIR)/ast/json_writer.c $(SRCDIR)/ast/program_dump.c
```
(`program_dump.c` is created in Task 2; add both now and create an empty `src/ast/program_dump.c` containing only `#include "program_dump.h"` plus the header from Task 2 Step 2 so the link succeeds.)

Insert `json-writer-test \` into `VERIFY_ALL_DEPS` immediately before `archive-determinism-probe \` (the list's tail, `Makefile:~3421`).

Run: `make lexer && make json-writer-test`
Expected: both succeed.

- [ ] **Step 7: Commit**

```bash
git add include/json_writer.h src/ast/json_writer.c tests/unit/ast/json_writer_test.c Makefile
git commit -m "feat(dump): deterministic streaming JSON writer with exact-output rows"
```

---

### Task 2: Dump skeleton, parse stage, and the probe with teeth

**Files:**
- Create: `include/program_dump.h`
- Create: `src/ast/program_dump.c` (replace the stub)
- Modify: `src/compiler/goo.c:64-65` (options), `:182-183` (help), `:257-258` (long options), `:275-278` (parsing), `:1366-1371` (parse-stage hook)
- Create: `scripts/program_dump_probe.sh`
- Create: `scripts/program_dump_check.py`
- Modify: `Makefile` (target + `VERIFY_ALL_DEPS`)

- [ ] **Step 1: Write the failing probe**

`scripts/program_dump_probe.sh`:

```bash
#!/usr/bin/env bash
# program-dump-probe — bin/goo --emit-ast-json (parse stage) and
# --emit-program (typed stage) over every golden fixture:
#   1. the dump is produced (exit 0) for every fixture the stage can reach;
#   2. two runs are byte-identical (determinism);
#   3. scripts/program_dump_check.py accepts the structure.
# Any AST or type kind the walker does not handle aborts bin/goo with the
# kind's name, so a fixture that reaches an unhandled kind is a red row here
# and names what is missing. That is the coverage proof for the format.
#
# Teeth (--self-test): GOO_DUMP_SELFTEST=nonce makes the walker emit a
# per-process nonce, so the determinism check MUST fail; GOO_DUMP_SELFTEST=nopos
# drops the first node's pos, so the structural check MUST fail.
set -u
cd "$(dirname "$0")/.."
GOO=${COMPILER:-./bin/goo}
CHECK="python3 scripts/program_dump_check.py"

run_fixtures() {
    # $1 = stage flag, $2 = fixture list (one path per line on stdin)
    local flag=$1 fails=0 n=0 tmp
    tmp=$(mktemp -d)
    while IFS= read -r f; do
        [ -z "$f" ] && continue
        n=$((n+1))
        local a="$tmp/a.json" b="$tmp/b.json"
        if ! "$GOO" "$flag" -o /dev/null "$f" > "$a" 2> "$tmp/err"; then
            echo "  FAIL  $f ($flag: exit $?)"; sed 's/^/        /' "$tmp/err" | head -3; fails=$((fails+1)); continue
        fi
        "$GOO" "$flag" -o /dev/null "$f" > "$b" 2>/dev/null
        if ! cmp -s "$a" "$b"; then echo "  FAIL  $f ($flag: two runs differ)"; fails=$((fails+1)); continue; fi
        if ! $CHECK "$a" > "$tmp/chk" 2>&1; then echo "  FAIL  $f ($flag: $(head -1 "$tmp/chk"))"; fails=$((fails+1)); continue; fi
    done
    rm -rf "$tmp"
    echo "  $flag: $n fixtures, $fails failed"
    return $((fails > 0))
}

parse_fixtures() {
    ls examples/*.expected.txt | sed 's/\.expected\.txt$/.goo/'
    ls tests/golden/reject/*.goo | grep -v -F -f <(grep -liE 'parse error|syntax error' tests/golden/reject/*.err.txt | sed 's/\.err\.txt$/.goo/')
}
typed_fixtures() { ls examples/*.expected.txt | sed 's/\.expected\.txt$/.goo/'; }

if [ "${1:-}" = "--self-test" ]; then
    f=examples/erru_catch_probe.goo
    tmp=$(mktemp -d)
    GOO_DUMP_SELFTEST=nonce "$GOO" --emit-ast-json -o /dev/null "$f" > "$tmp/1"
    GOO_DUMP_SELFTEST=nonce "$GOO" --emit-ast-json -o /dev/null "$f" > "$tmp/2"
    if cmp -s "$tmp/1" "$tmp/2"; then echo "program-dump-probe --self-test: FAIL (nonce did not change the dump)"; exit 1; fi
    GOO_DUMP_SELFTEST=nopos "$GOO" --emit-ast-json -o /dev/null "$f" > "$tmp/3"
    if $CHECK "$tmp/3" >/dev/null 2>&1; then echo "program-dump-probe --self-test: FAIL (checker accepted a node without pos)"; exit 1; fi
    rm -rf "$tmp"
    echo "program-dump-probe --self-test: PASS (nonce breaks determinism, missing pos is refused)"
    exit 0
fi

ok=1
echo "=== program-dump-probe: dump every fixture at both stages ==="
parse_fixtures | run_fixtures --emit-ast-json || ok=0
typed_fixtures | run_fixtures --emit-program || ok=0
if [ $ok = 1 ]; then echo "program-dump-probe: PASS"; else echo "program-dump-probe: FAIL"; exit 1; fi
```

`scripts/program_dump_check.py`:

```python
#!/usr/bin/env python3
"""Structural check for a goo program dump. Exit 1 with one line naming the
first problem. Kept independent of the kind list on purpose: it checks the
SHAPE every node and type must have, and that every id resolves, so a new
kind needs no change here and a malformed one is still refused."""
import json, sys

EXPR_KINDS = {
    "IDENTIFIER", "LITERAL", "BINARY_EXPR", "UNARY_EXPR", "POSTFIX_EXPR",
    "CALL_EXPR", "INDEX_EXPR", "SELECTOR_EXPR", "SLICE_LIT", "MAP_LIT",
    "SLICE_INDEX_EXPR", "STRUCT_LITERAL", "ARRAY_LITERAL", "FUNC_LIT",
    "SLICE_CONVERSION", "TYPE_ASSERT", "TRY_EXPR", "CATCH_EXPR", "ADDR_OF",
    "PTR_DEREF", "MATCH_EXPR",
}

def fail(msg):
    print(msg); sys.exit(1)

def walk(node, typed, ntypes, path):
    if isinstance(node, list):
        for i, n in enumerate(node): walk(n, typed, ntypes, f"{path}[{i}]")
        return
    if not isinstance(node, dict): return
    if "kind" in node:
        k = node["kind"]
        if not isinstance(k, str) or not k: fail(f"{path}: kind is not a string")
        p = node.get("pos")
        if not (isinstance(p, list) and len(p) == 3 and all(isinstance(x, int) for x in p)):
            fail(f"{path} ({k}): pos must be [line, column, offset]")
        if "type" in node:
            t = node["type"]
            if not isinstance(t, int) or t < 0 or t >= ntypes: fail(f"{path} ({k}): type id {t} does not resolve")
        elif typed and k in EXPR_KINDS:
            fail(f"{path} ({k}): typed stage but no type id")
    for key, v in node.items():
        if key in ("kind", "pos", "type"): continue
        walk(v, typed, ntypes, f"{path}.{key}")

def check_types(types):
    for i, t in enumerate(types):
        if t.get("id") != i: fail(f"types[{i}]: id {t.get('id')} out of order")
        if not isinstance(t.get("kind"), str): fail(f"types[{i}]: kind missing")
        for key, v in t.items():
            refs = v if isinstance(v, list) else [v]
            for r in refs:
                if key.endswith("_type") or key in ("element", "key", "value", "pointee", "referenced", "base", "return", "constraint", "payload"):
                    if isinstance(r, int) and not (0 <= r < len(types)): fail(f"types[{i}].{key}: id {r} does not resolve")
                if isinstance(r, dict) and "type" in r and isinstance(r["type"], int) and not (0 <= r["type"] < len(types)):
                    fail(f"types[{i}].{key}: member type id {r['type']} does not resolve")

def main():
    with open(sys.argv[1]) as fh: d = json.load(fh)
    if d.get("goo_program_dump") != 1: fail("missing goo_program_dump: 1")
    stage = d.get("stage")
    if stage not in ("parse", "typed"): fail(f"bad stage {stage!r}")
    types = d.get("types")
    if not isinstance(types, list): fail("types must be a list")
    if stage == "parse" and types: fail("parse stage must have an empty type table")
    check_types(types)
    files = d.get("files")
    if not isinstance(files, list) or not files: fail("files must be a non-empty list")
    for fi, f in enumerate(files):
        for k in ("file", "package", "imports", "decls", "plan"):
            if k not in f: fail(f"files[{fi}]: missing {k}")
        walk(f["imports"], stage == "typed", len(types), f"files[{fi}].imports")
        walk(f["decls"], stage == "typed", len(types), f"files[{fi}].decls")
        if stage == "parse" and f["plan"] is not None: fail(f"files[{fi}]: parse stage must have plan: null")
    print("ok")

if __name__ == "__main__": main()
```

- [ ] **Step 2: Run the probe to verify it fails**

Run: `chmod +x scripts/program_dump_probe.sh && bash scripts/program_dump_probe.sh --self-test`
Expected: FAIL (`--emit-ast-json` is an unknown option, so the two files are identical, and the message is "nonce did not change the dump").

- [ ] **Step 3: Write the dump header**

`include/program_dump.h`:

```c
#ifndef PROGRAM_DUMP_H
#define PROGRAM_DUMP_H

#include "ast.h"
#include "release_decision.h"
#include <stdio.h>
#include <stddef.h>

typedef enum {
    PROGRAM_DUMP_PARSE,   // after parsing: no type ids, plan is null
    PROGRAM_DUMP_TYPED,   // after type_check_program_files: type ids + plan
} ProgramDumpStage;

// Writes the dump for `nfiles` parsed (or checked) files. `plans` may be NULL,
// or an array of `nfiles` ReleasePlan* where an entry may be NULL (ARC off).
// Aborts, naming the kind, on any AST or type kind it does not handle: the
// fixture gate (scripts/program_dump_probe.sh) is the coverage proof.
void program_dump_write(FILE* out, ASTNode** files, const char** filenames,
                        size_t nfiles, ReleasePlan** plans, ProgramDumpStage stage);

#endif
```

- [ ] **Step 4: Write the walker skeleton with the file envelope and the abort path**

`src/ast/program_dump.c` (this version handles only PROGRAM/PACKAGE_DECL/IMPORT_SPEC/FUNC_DECL; Tasks 3–5 fill in the rest of the switch):

```c
// program_dump: the typed-program interchange the front-end migration is
// measured against. One JSON object per node, a type table by id, one plan
// per file. Every kind the walker does not know aborts BY NAME — a dump that
// silently skipped a node would let a differential gate pass on a fixture
// it never actually compared.
#include "program_dump.h"
#include "json_writer.h"
#include "types.h"
#include "escape_core.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ---- type table ------------------------------------------------------------

typedef struct {
    Type** items;
    size_t count, cap;
} TypeTable;

static TypeTable g_types;

static long long type_id(Type* t) {
    if (!t) return -1;
    for (size_t i = 0; i < g_types.count; i++) if (g_types.items[i] == t) return (long long)i;
    if (g_types.count == g_types.cap) {
        g_types.cap = g_types.cap ? g_types.cap * 2 : 64;
        g_types.items = realloc(g_types.items, g_types.cap * sizeof(Type*));
        if (!g_types.items) abort();
    }
    g_types.items[g_types.count] = t;
    return (long long)g_types.count++;
}

// ---- helpers ---------------------------------------------------------------

static ProgramDumpStage g_stage;
static const char* g_selftest;   // GOO_DUMP_SELFTEST, or NULL
static int g_nodes_emitted;

static void die_kind(const char* what, int kind) {
    fprintf(stderr, "program-dump: unsupported %s kind %d\n", what, kind);
    abort();
}

static void emit_node(JsonW* w, ASTNode* n);

static void emit_list(JsonW* w, const char* key, ASTNode* head) {
    jw_key(w, key);
    jw_begin_array(w);
    for (ASTNode* n = head; n; n = n->next) emit_node(w, n);
    jw_end_array(w);
}

static void emit_child(JsonW* w, const char* key, ASTNode* n) {
    jw_key(w, key);
    if (n) emit_node(w, n); else jw_null(w);
}

static void emit_str(JsonW* w, const char* key, const char* s) { jw_key(w, key); jw_string(w, s); }
static void emit_int(JsonW* w, const char* key, long long v)   { jw_key(w, key); jw_int(w, v); }
static void emit_bool(JsonW* w, const char* key, int v)        { jw_key(w, key); jw_bool(w, v != 0); }

static void emit_names(JsonW* w, const char* key, char** names, size_t count) {
    jw_key(w, key);
    jw_begin_array(w);
    for (size_t i = 0; i < count; i++) jw_string(w, names[i]);
    jw_end_array(w);
}

// Opens the node object and writes the fields every node has. The caller
// writes the kind-specific fields and closes the object.
static void begin_node(JsonW* w, ASTNode* n, const char* kind) {
    jw_begin_object(w);
    emit_str(w, "kind", kind);
    // Teeth for the probe's structural check: drop pos from the first node.
    if (!(g_selftest && strcmp(g_selftest, "nopos") == 0 && g_nodes_emitted == 0)) {
        jw_key(w, "pos");
        jw_begin_array(w);
        jw_int(w, n->pos.line); jw_int(w, n->pos.column); jw_int(w, n->pos.offset);
        jw_end_array(w);
    }
    g_nodes_emitted++;
    if (g_stage == PROGRAM_DUMP_TYPED && n->node_type) emit_int(w, "type", type_id(n->node_type));
}

static const char* ownership_name(OwnershipKind k) {
    switch (k) {
        case OWNERSHIP_NONE: return "OWNERSHIP_NONE";
        case OWNERSHIP_OWNED: return "OWNERSHIP_OWNED";
        case OWNERSHIP_BORROWED: return "OWNERSHIP_BORROWED";
        case OWNERSHIP_SHARED: return "OWNERSHIP_SHARED";
    }
    die_kind("ownership", k); return NULL;
}

// ---- nodes -----------------------------------------------------------------

static void emit_node(JsonW* w, ASTNode* n) {
    switch (n->type) {
        case AST_PACKAGE_DECL: {
            PackageDeclNode* p = (PackageDeclNode*)n;
            begin_node(w, n, "PACKAGE_DECL"); emit_str(w, "name", p->name); jw_end_object(w); break;
        }
        case AST_IMPORT_SPEC: {
            ImportSpecNode* p = (ImportSpecNode*)n;
            begin_node(w, n, "IMPORT_SPEC"); emit_str(w, "path", p->path); emit_str(w, "alias", p->alias); jw_end_object(w); break;
        }
        case AST_FUNC_DECL: {
            FuncDeclNode* f = (FuncDeclNode*)n;
            begin_node(w, n, "FUNC_DECL");
            emit_str(w, "name", f->name);
            emit_bool(w, "is_comptime", f->is_comptime);
            emit_bool(w, "is_unsafe", f->is_unsafe);
            emit_bool(w, "has_receiver", f->receiver != NULL);   // receiver is params[0]
            emit_list(w, "type_params", f->type_params);
            emit_list(w, "params", f->params);
            emit_child(w, "return_type", f->return_type);
            emit_list(w, "annotations", f->annotations);
            emit_child(w, "body", f->body);
            jw_end_object(w); break;
        }
        // Tasks 3-5 add every other live kind here.
        default:
            die_kind("AST node", n->type);
    }
}

// ---- types (Task 5 fills emit_type_entry) ----------------------------------

static void emit_type_entry(JsonW* w, size_t id, Type* t);

static void emit_type_table(JsonW* w) {
    jw_key(w, "types");
    jw_begin_array(w);
    // Component types are appended to g_types while entries are emitted, so
    // loop on the live count until the table is closed under reference.
    for (size_t i = 0; i < g_types.count; i++) emit_type_entry(w, i, g_types.items[i]);
    jw_end_array(w);
}

// ---- plan (Task 5 fills emit_plan) ----------------------------------------

static void emit_plan(JsonW* w, ReleasePlan* plan);

// ---- entry -----------------------------------------------------------------

void program_dump_write(FILE* out, ASTNode** files, const char** filenames,
                        size_t nfiles, ReleasePlan** plans, ProgramDumpStage stage) {
    JsonW w; jw_init(&w, out);
    g_stage = stage;
    g_selftest = getenv("GOO_DUMP_SELFTEST");
    g_nodes_emitted = 0;
    g_types.count = 0;

    jw_begin_object(&w);
    emit_int(&w, "goo_program_dump", 1);
    emit_str(&w, "stage", stage == PROGRAM_DUMP_PARSE ? "parse" : "typed");
    if (g_selftest && strcmp(g_selftest, "nonce") == 0) emit_int(&w, "nonce", (long long)getpid());
    jw_key(&w, "files");
    jw_begin_array(&w);
    for (size_t i = 0; i < nfiles; i++) {
        ProgramNode* p = (ProgramNode*)files[i];
        jw_begin_object(&w);
        emit_str(&w, "file", filenames[i]);
        emit_str(&w, "package", p->package_name);
        emit_list(&w, "imports", p->imports);
        emit_list(&w, "decls", p->decls);
        jw_key(&w, "plan");
        if (stage == PROGRAM_DUMP_TYPED && plans && plans[i]) emit_plan(&w, plans[i]); else jw_null(&w);
        jw_end_object(&w);
    }
    jw_end_array(&w);
    emit_type_table(&w);
    jw_end_object(&w);
    fputc('\n', out);
    fflush(out);
}

// Placeholders that Task 5 replaces. They exist so this task links; the typed
// stage is not wired into the driver until Task 5, so neither is reachable.
static void emit_type_entry(JsonW* w, size_t id, Type* t) { (void)w; (void)id; (void)t; abort(); }
static void emit_plan(JsonW* w, ReleasePlan* plan)          { (void)w; (void)plan; abort(); }
```

- [ ] **Step 5: Wire the parse-stage flag into the driver**

`src/compiler/goo.c`:

At `:64-65` add `bool emit_ast_json;` and `bool emit_program;` beside `emit_ast`.

At `:182-183` add help lines:
```c
    fprintf(out, "  --emit-ast-json          Emit the parsed program as JSON (program dump, parse stage)\n");
    fprintf(out, "  --emit-program           Emit the typed program + release plan as JSON (program dump)\n");
```

At `:257-258` add to `long_options`:
```c
        {"emit-ast-json", no_argument, 0, 0},
        {"emit-program", no_argument, 0, 0},
```

At `:275-278` add the two branches:
```c
                } else if (strcmp(long_options[option_index].name, "emit-ast-json") == 0) {
                    options->emit_ast_json = true;
                } else if (strcmp(long_options[option_index].name, "emit-program") == 0) {
                    options->emit_program = true;
```

Directly after the parse loop closes (after the `}` that ends the `if (options->emit_ast) {...}` block's enclosing loop at `:1371`), before the `dump_packages` short-circuit:
```c
    if (options->emit_ast_json) {
        program_dump_write(stdout, asts, (const char**)filenames, nfiles, NULL, PROGRAM_DUMP_PARSE);
        ENTRY_CLEANUP();
        return true;
    }
```
`filenames` is whatever array holds the per-file source names in that function (the `fname` passed to `lexer_new` at `:1143` comes from it); if the names live in a differently named array, use that name. Add `#include "program_dump.h"` at the top of `goo.c`.

- [ ] **Step 6: Build and run the self-test**

Run: `make lexer && bash scripts/program_dump_probe.sh --self-test`
Expected: `program-dump-probe --self-test: PASS (nonce breaks determinism, missing pos is refused)`.

- [ ] **Step 7: Run the full probe to see the first unsupported kind**

Run: `bash scripts/program_dump_probe.sh 2>&1 | head -8`
Expected: the parse-stage rows fail with `program-dump: unsupported AST node kind N` for the first statement kind reached (a `BLOCK_STMT`, kind 9). That is the red state Task 3 turns green.

- [ ] **Step 8: Add the Makefile target and gate entry**

After the `alloc-doors-probe` stanza (`Makefile:5327`):
```make
# Phase 0 (front-end migration): the program dump is the interchange every
# later differential gate compares. Determinism + structure over every
# fixture, and an abort-by-name on any kind the walker does not cover.
program-dump-probe: $(COMPILER) $(RUNTIME_LIB)
	@bash scripts/program_dump_probe.sh

program-dump-selftest:
	@bash scripts/program_dump_probe.sh --self-test
```
Add `program-dump-selftest \` and `program-dump-probe \` to `VERIFY_ALL_DEPS` before `archive-determinism-probe \`. (The probe is red until Task 4; commit the gate now so the red is visible, and land Tasks 3–4 on the same branch before merging.)

- [ ] **Step 9: Commit**

```bash
git add include/program_dump.h src/ast/program_dump.c src/compiler/goo.c scripts/program_dump_probe.sh scripts/program_dump_check.py Makefile
git commit -m "feat(dump): --emit-ast-json skeleton, structural checker, probe with teeth"
```

---

### Task 3: Declarations and statements

**Files:**
- Modify: `src/ast/program_dump.c` (the `emit_node` switch)

- [ ] **Step 1: Record the red state**

Run: `bash scripts/program_dump_probe.sh 2>&1 | grep -c FAIL`
Expected: a positive count (every fixture, since all have a function body).

- [ ] **Step 2: Add the declaration and statement cases**

Insert into the `emit_node` switch, before `default:`:

```c
        case AST_VAR_DECL: {
            VarDeclNode* v = (VarDeclNode*)n;
            begin_node(w, n, "VAR_DECL");
            emit_names(w, "names", v->names, v->name_count);
            emit_child(w, "decl_type", v->type);
            emit_list(w, "values", v->values);
            emit_str(w, "ownership", ownership_name(v->ownership));
            emit_bool(w, "is_short_decl", v->is_short_decl);
            emit_bool(w, "is_variadic_param", v->is_variadic_param);
            emit_bool(w, "is_captured", v->is_captured);
            emit_bool(w, "is_embedded", v->is_embedded);
            emit_bool(w, "is_comptime_param", v->is_comptime_param);
            jw_end_object(w); break;
        }
        case AST_CONST_DECL: {
            ConstDeclNode* c = (ConstDeclNode*)n;
            begin_node(w, n, "CONST_DECL");
            emit_names(w, "names", c->names, c->name_count);
            emit_child(w, "decl_type", c->type);
            emit_list(w, "values", c->values);
            emit_bool(w, "is_comptime", c->is_comptime);
            jw_end_object(w); break;
        }
        case AST_TYPE_DECL: {
            TypeDeclNode* t = (TypeDeclNode*)n;
            begin_node(w, n, "TYPE_DECL"); emit_str(w, "name", t->name); emit_child(w, "decl_type", t->type); jw_end_object(w); break;
        }
        case AST_EXTERN_DECL: {
            ExternDeclNode* e = (ExternDeclNode*)n;
            begin_node(w, n, "EXTERN_DECL");
            emit_str(w, "name", e->name); emit_str(w, "abi", e->abi);
            emit_list(w, "params", e->params); emit_child(w, "return_type", e->return_type);
            emit_str(w, "library", e->library);
            jw_end_object(w); break;
        }
        case AST_ATTRIBUTE: {
            AttributeNode* a = (AttributeNode*)n;
            begin_node(w, n, "ATTRIBUTE"); emit_str(w, "name", a->name); emit_list(w, "args", a->args); jw_end_object(w); break;
        }
        case AST_BLOCK_STMT: {
            begin_node(w, n, "BLOCK_STMT"); emit_list(w, "statements", ((BlockStmtNode*)n)->statements); jw_end_object(w); break;
        }
        case AST_EXPR_STMT: {
            begin_node(w, n, "EXPR_STMT"); emit_child(w, "expr", ((ExprStmtNode*)n)->expr); jw_end_object(w); break;
        }
        case AST_IF_STMT: {
            IfStmtNode* s = (IfStmtNode*)n;
            begin_node(w, n, "IF_STMT");
            emit_child(w, "condition", s->condition); emit_child(w, "then", s->then_stmt); emit_child(w, "else", s->else_stmt);
            jw_end_object(w); break;
        }
        case AST_IF_LET_STMT: {
            IfLetStmtNode* s = (IfLetStmtNode*)n;
            begin_node(w, n, "IF_LET_STMT");
            emit_str(w, "var_name", s->var_name); emit_child(w, "nullable_expr", s->nullable_expr);
            emit_child(w, "then", s->then_stmt); emit_child(w, "else", s->else_stmt);
            jw_end_object(w); break;
        }
        case AST_FOR_STMT: {
            ForStmtNode* s = (ForStmtNode*)n;
            begin_node(w, n, "FOR_STMT");
            emit_child(w, "init", s->init); emit_child(w, "condition", s->condition); emit_child(w, "post", s->post);
            emit_child(w, "range_expr", s->range_expr); emit_str(w, "key_name", s->key_name); emit_str(w, "value_name", s->value_name);
            emit_child(w, "body", s->body);
            jw_end_object(w); break;
        }
        case AST_RETURN_STMT: {
            begin_node(w, n, "RETURN_STMT"); emit_list(w, "values", ((ReturnStmtNode*)n)->values); jw_end_object(w); break;
        }
        case AST_BREAK_STMT:       begin_node(w, n, "BREAK_STMT"); jw_end_object(w); break;
        case AST_CONTINUE_STMT:    begin_node(w, n, "CONTINUE_STMT"); jw_end_object(w); break;
        case AST_FALLTHROUGH_STMT: begin_node(w, n, "FALLTHROUGH_STMT"); jw_end_object(w); break;
        case AST_DEFER_STMT: {
            begin_node(w, n, "DEFER_STMT"); emit_child(w, "call", ((DeferStmtNode*)n)->call); jw_end_object(w); break;
        }
        case AST_GO_STMT: {
            begin_node(w, n, "GO_STMT"); emit_child(w, "call", ((GoStmtNode*)n)->call); jw_end_object(w); break;
        }
        case AST_SELECT_STMT: {
            begin_node(w, n, "SELECT_STMT"); emit_list(w, "cases", ((SelectStmtNode*)n)->cases); jw_end_object(w); break;
        }
        case AST_SELECT_CASE: {
            SelectCaseNode* c = (SelectCaseNode*)n;
            begin_node(w, n, "SELECT_CASE");
            emit_child(w, "comm", c->comm); emit_list(w, "body", c->body);
            emit_str(w, "bind_name", c->bind_name); emit_int(w, "is_declare", c->is_declare);
            jw_end_object(w); break;
        }
        case AST_SWITCH_STMT: {
            SwitchStmtNode* s = (SwitchStmtNode*)n;
            begin_node(w, n, "SWITCH_STMT"); emit_child(w, "tag", s->tag); emit_list(w, "cases", s->cases); jw_end_object(w); break;
        }
        case AST_CASE_CLAUSE: {
            CaseClauseNode* c = (CaseClauseNode*)n;
            begin_node(w, n, "CASE_CLAUSE"); emit_list(w, "exprs", c->exprs); emit_list(w, "body", c->body); jw_end_object(w); break;
        }
        case AST_TYPE_SWITCH: {
            TypeSwitchNode* s = (TypeSwitchNode*)n;
            begin_node(w, n, "TYPE_SWITCH");
            emit_child(w, "bind_name", s->bind_name); emit_child(w, "expr", s->expr); emit_list(w, "cases", s->cases);
            jw_end_object(w); break;
        }
        case AST_TYPE_CASE: {
            TypeCaseNode* c = (TypeCaseNode*)n;
            begin_node(w, n, "TYPE_CASE"); emit_list(w, "types", c->types); emit_list(w, "body", c->body); jw_end_object(w); break;
        }
        case AST_UNSAFE_STMT: {
            begin_node(w, n, "UNSAFE_STMT"); emit_child(w, "body", ((UnsafeStmtNode*)n)->body); jw_end_object(w); break;
        }
        case AST_ARENA_BLOCK: {
            begin_node(w, n, "ARENA_BLOCK"); emit_child(w, "body", ((ArenaBlockNode*)n)->body); jw_end_object(w); break;
        }
        case AST_COMPTIME_BLOCK: {
            begin_node(w, n, "COMPTIME_BLOCK"); emit_child(w, "body", ((ComptimeBlockNode*)n)->body); jw_end_object(w); break;
        }
        case AST_LABEL_STMT: {
            LabelStmtNode* s = (LabelStmtNode*)n;
            begin_node(w, n, "LABEL_STMT"); emit_str(w, "name", s->name); emit_child(w, "stmt", s->stmt); jw_end_object(w); break;
        }
        case AST_BREAK_LABEL_STMT: {
            begin_node(w, n, "BREAK_LABEL_STMT"); emit_str(w, "label", ((BreakLabelStmtNode*)n)->label); jw_end_object(w); break;
        }
        case AST_CONTINUE_LABEL_STMT: {
            begin_node(w, n, "CONTINUE_LABEL_STMT"); emit_str(w, "label", ((ContinueLabelStmtNode*)n)->label); jw_end_object(w); break;
        }
        case AST_GOTO_STMT: {
            begin_node(w, n, "GOTO_STMT"); emit_str(w, "label", ((GotoStmtNode*)n)->label); jw_end_object(w); break;
        }
        case AST_MULTI_ASSIGN: {
            MultiAssignNode* m = (MultiAssignNode*)n;
            begin_node(w, n, "MULTI_ASSIGN");
            emit_list(w, "targets", m->targets); emit_list(w, "values", m->values);
            emit_int(w, "count", (long long)m->count); emit_bool(w, "is_short_decl", m->is_short_decl);
            jw_end_object(w); break;
        }
```

- [ ] **Step 3: Run the probe**

Run: `make lexer && bash scripts/program_dump_probe.sh 2>&1 | grep -m3 'unsupported'`
Expected: the first unsupported kind is now an expression or type kind (`IDENTIFIER` is 26, `BASIC_TYPE` is 37).

- [ ] **Step 4: Commit**

```bash
git add src/ast/program_dump.c
git commit -m "feat(dump): declarations and statements"
```

---

### Task 4: Expressions and type expressions — parse stage goes green

**Files:**
- Modify: `src/ast/program_dump.c`

- [ ] **Step 1: Add the token-name helper and enumeration names**

Above `emit_node`:

```c
#include "token.h"

static const char* pattern_name(PatternType p) {
    switch (p) {
        case PATTERN_LITERAL: return "PATTERN_LITERAL";
        case PATTERN_IDENTIFIER: return "PATTERN_IDENTIFIER";
        case PATTERN_WILDCARD: return "PATTERN_WILDCARD";
        case PATTERN_DESTRUCTURE: return "PATTERN_DESTRUCTURE";
        case PATTERN_TYPE: return "PATTERN_TYPE";
        case PATTERN_OR: return "PATTERN_OR";
    }
    die_kind("pattern", p); return NULL;
}

static const char* chan_pattern_name(ChannelPattern p) {
    switch (p) {
        case CHAN_PATTERN_BASIC: return "CHAN_PATTERN_BASIC";
        case CHAN_PATTERN_PUB: return "CHAN_PATTERN_PUB";
        case CHAN_PATTERN_SUB: return "CHAN_PATTERN_SUB";
        case CHAN_PATTERN_REQ: return "CHAN_PATTERN_REQ";
        case CHAN_PATTERN_REP: return "CHAN_PATTERN_REP";
        case CHAN_PATTERN_PUSH: return "CHAN_PATTERN_PUSH";
        case CHAN_PATTERN_PULL: return "CHAN_PATTERN_PULL";
    }
    die_kind("channel pattern", p); return NULL;
}

// token_type_string is the lexer's own name table (src/lexer/token.c:247);
// reusing it keeps the operator spelling identical to --emit-tokens.
static void emit_tok(JsonW* w, const char* key, TokenType t) { emit_str(w, key, token_type_string(t)); }
```

- [ ] **Step 2: Add the expression cases**

```c
        case AST_IDENTIFIER: {
            begin_node(w, n, "IDENTIFIER"); emit_str(w, "name", ((IdentifierNode*)n)->name); jw_end_object(w); break;
        }
        case AST_LITERAL: {
            LiteralNode* l = (LiteralNode*)n;
            begin_node(w, n, "LITERAL");
            emit_tok(w, "literal_type", l->literal_type);
            jw_key(w, "value"); jw_string_len(w, l->value, l->length);
            emit_int(w, "length", (long long)l->length);
            jw_end_object(w); break;
        }
        case AST_BINARY_EXPR: {
            BinaryExprNode* b = (BinaryExprNode*)n;
            begin_node(w, n, "BINARY_EXPR"); emit_tok(w, "op", b->operator); emit_child(w, "left", b->left); emit_child(w, "right", b->right); jw_end_object(w); break;
        }
        case AST_UNARY_EXPR: {
            UnaryExprNode* u = (UnaryExprNode*)n;
            begin_node(w, n, "UNARY_EXPR"); emit_tok(w, "op", u->operator); emit_child(w, "operand", u->operand); jw_end_object(w); break;
        }
        case AST_POSTFIX_EXPR: {
            PostfixExprNode* p = (PostfixExprNode*)n;
            begin_node(w, n, "POSTFIX_EXPR"); emit_tok(w, "op", p->operator); emit_child(w, "operand", p->operand); jw_end_object(w); break;
        }
        case AST_CALL_EXPR: {
            CallExprNode* c = (CallExprNode*)n;
            begin_node(w, n, "CALL_EXPR");
            emit_child(w, "function", c->function); emit_list(w, "args", c->args);
            emit_bool(w, "has_spread", c->has_spread);
            jw_key(w, "type_args"); jw_begin_array(w);
            if (g_stage == PROGRAM_DUMP_TYPED) for (size_t i = 0; i < c->type_arg_count; i++) jw_int(w, type_id(c->type_args[i]));
            jw_end_array(w);
            jw_key(w, "comptime_value_args"); jw_begin_array(w);
            for (size_t i = 0; i < c->comptime_value_arg_count; i++) jw_int(w, (long long)c->comptime_value_args[i]);
            jw_end_array(w);
            jw_end_object(w); break;
        }
        case AST_INDEX_EXPR: {
            IndexExprNode* e = (IndexExprNode*)n;
            begin_node(w, n, "INDEX_EXPR"); emit_child(w, "expr", e->expr); emit_child(w, "index", e->index); jw_end_object(w); break;
        }
        case AST_SELECTOR_EXPR: {
            SelectorExprNode* e = (SelectorExprNode*)n;
            begin_node(w, n, "SELECTOR_EXPR"); emit_child(w, "expr", e->expr); emit_str(w, "selector", e->selector); jw_end_object(w); break;
        }
        case AST_SLICE_EXPR: {   // SliceLitNode: the enum slot is reused for slice literals
            SliceLitNode* s = (SliceLitNode*)n;
            begin_node(w, n, "SLICE_LIT"); emit_list(w, "elements", s->elements); emit_child(w, "elem_type", s->elem_type); jw_end_object(w); break;
        }
        case AST_PAREN_EXPR: {   // MapLitNode: the enum slot is reused for map literals
            MapLitNode* m = (MapLitNode*)n;
            begin_node(w, n, "MAP_LIT"); emit_child(w, "map_type", m->map_type); emit_list(w, "keys", m->keys); emit_list(w, "values", m->values); jw_end_object(w); break;
        }
        case AST_SLICE_INDEX_EXPR: {
            SliceIndexExprNode* e = (SliceIndexExprNode*)n;
            begin_node(w, n, "SLICE_INDEX_EXPR"); emit_child(w, "expr", e->expr); emit_child(w, "low", e->low); emit_child(w, "high", e->high); jw_end_object(w); break;
        }
        case AST_STRUCT_LITERAL: {
            StructLiteralNode* s = (StructLiteralNode*)n;
            begin_node(w, n, "STRUCT_LITERAL");
            emit_str(w, "type_name", s->type_name); emit_bool(w, "is_keyed", s->is_keyed);
            jw_key(w, "field_names"); jw_begin_array(w);
            if (s->field_names) for (size_t i = 0; i < s->field_count; i++) jw_string(w, s->field_names[i]);
            jw_end_array(w);
            emit_list(w, "field_values", s->field_values);
            emit_int(w, "field_count", (long long)s->field_count);
            jw_end_object(w); break;
        }
        case AST_ARRAY_LITERAL: {
            ArrayLitNode* a = (ArrayLitNode*)n;
            begin_node(w, n, "ARRAY_LITERAL"); emit_child(w, "array_type", a->array_type); emit_list(w, "elements", a->elements); jw_end_object(w); break;
        }
        case AST_KEYED_ELEMENT: {
            KeyedElementNode* k = (KeyedElementNode*)n;
            begin_node(w, n, "KEYED_ELEMENT"); emit_child(w, "key", k->key); emit_child(w, "value", k->value); jw_end_object(w); break;
        }
        case AST_FUNC_LIT: {
            FuncLitNode* f = (FuncLitNode*)n;
            begin_node(w, n, "FUNC_LIT");
            emit_list(w, "params", f->params); emit_child(w, "return_type", f->return_type); emit_child(w, "body", f->body);
            emit_names(w, "captured_names", f->captured_names, f->captured_count);
            jw_end_object(w); break;
        }
        case AST_SLICE_CONVERSION: {
            SliceConvNode* s = (SliceConvNode*)n;
            begin_node(w, n, "SLICE_CONVERSION"); emit_child(w, "slice_type", s->slice_type); emit_child(w, "operand", s->operand); jw_end_object(w); break;
        }
        case AST_TYPE_ASSERT: {
            TypeAssertNode* t = (TypeAssertNode*)n;
            begin_node(w, n, "TYPE_ASSERT"); emit_child(w, "expr", t->expr); emit_child(w, "asserted_type", t->asserted_type); jw_end_object(w); break;
        }
        case AST_TRY_EXPR: {
            begin_node(w, n, "TRY_EXPR"); emit_child(w, "expr", ((TryExprNode*)n)->expr); jw_end_object(w); break;
        }
        case AST_CATCH_EXPR: {
            CatchExprNode* c = (CatchExprNode*)n;
            begin_node(w, n, "CATCH_EXPR"); emit_child(w, "expr", c->expr); emit_str(w, "error_var", c->error_var); emit_child(w, "catch_body", c->catch_body); jw_end_object(w); break;
        }
        case AST_ADDR_OF: {
            begin_node(w, n, "ADDR_OF"); emit_child(w, "operand", ((AddrOfNode*)n)->operand); jw_end_object(w); break;
        }
        case AST_PTR_DEREF: {
            begin_node(w, n, "PTR_DEREF"); emit_child(w, "pointer", ((PtrDerefNode*)n)->pointer); jw_end_object(w); break;
        }
        case AST_MATCH_EXPR: {
            MatchExprNode* m = (MatchExprNode*)n;
            begin_node(w, n, "MATCH_EXPR"); emit_child(w, "expr", m->expr); emit_list(w, "cases", m->cases); jw_end_object(w); break;
        }
        case AST_MATCH_CASE: {
            MatchCaseNode* c = (MatchCaseNode*)n;
            begin_node(w, n, "MATCH_CASE"); emit_child(w, "pattern", c->pattern); emit_child(w, "guard", c->guard); emit_list(w, "body", c->body); jw_end_object(w); break;
        }
        case AST_GUARD_CONDITION: {
            begin_node(w, n, "GUARD_CONDITION"); emit_child(w, "condition", ((GuardConditionNode*)n)->condition); jw_end_object(w); break;
        }
        case AST_PATTERN: {
            PatternNode* p = (PatternNode*)n;
            begin_node(w, n, "PATTERN");
            emit_str(w, "pattern_type", pattern_name(p->pattern_type));
            switch (p->pattern_type) {
                case PATTERN_LITERAL: emit_child(w, "literal", p->data.literal.literal); break;
                case PATTERN_IDENTIFIER: emit_str(w, "name", p->data.identifier.name); emit_child(w, "id_type", p->data.identifier.type); break;
                case PATTERN_WILDCARD: break;
                case PATTERN_DESTRUCTURE:
                case PATTERN_TYPE: emit_str(w, "type_name", p->data.destructure.type_name); emit_list(w, "fields", p->data.destructure.fields); break;
                case PATTERN_OR: emit_list(w, "patterns", p->data.or_pattern.patterns); break;
            }
            jw_end_object(w); break;
        }
```

- [ ] **Step 3: Add the type-expression cases**

```c
        case AST_BASIC_TYPE: {
            BasicTypeNode* t = (BasicTypeNode*)n;
            begin_node(w, n, "BASIC_TYPE"); emit_str(w, "name", t->name); emit_str(w, "package", t->package); jw_end_object(w); break;
        }
        case AST_ARRAY_TYPE: {
            ArrayTypeNode* t = (ArrayTypeNode*)n;
            begin_node(w, n, "ARRAY_TYPE"); emit_child(w, "length", t->length); emit_child(w, "element_type", t->element_type); jw_end_object(w); break;
        }
        case AST_SLICE_TYPE: {
            begin_node(w, n, "SLICE_TYPE"); emit_child(w, "element_type", ((SliceTypeNode*)n)->element_type); jw_end_object(w); break;
        }
        case AST_MAP_TYPE: {
            MapTypeNode* t = (MapTypeNode*)n;
            begin_node(w, n, "MAP_TYPE"); emit_child(w, "key_type", t->key_type); emit_child(w, "value_type", t->value_type); jw_end_object(w); break;
        }
        case AST_CHAN_TYPE: {
            ChanTypeNode* t = (ChanTypeNode*)n;
            begin_node(w, n, "CHAN_TYPE"); emit_child(w, "element_type", t->element_type);
            emit_str(w, "pattern", chan_pattern_name(t->pattern)); emit_str(w, "endpoint", t->endpoint);
            jw_end_object(w); break;
        }
        case AST_FUNC_TYPE: {
            FuncTypeNode* t = (FuncTypeNode*)n;
            begin_node(w, n, "FUNC_TYPE"); emit_list(w, "params", t->params); emit_child(w, "return_type", t->return_type); jw_end_object(w); break;
        }
        case AST_INTERFACE_TYPE: {
            begin_node(w, n, "INTERFACE_TYPE"); emit_list(w, "methods", ((InterfaceTypeNode*)n)->methods); jw_end_object(w); break;
        }
        case AST_STRUCT_TYPE: {
            StructTypeNode* t = (StructTypeNode*)n;
            begin_node(w, n, "STRUCT_TYPE"); emit_list(w, "fields", t->fields); emit_bool(w, "is_result_tuple", t->is_result_tuple); jw_end_object(w); break;
        }
        case AST_ENUM_TYPE: {
            begin_node(w, n, "ENUM_TYPE"); emit_list(w, "variants", ((EnumTypeNode*)n)->variants); jw_end_object(w); break;
        }
        case AST_ENUM_VARIANT: {
            EnumVariantNode* v = (EnumVariantNode*)n;
            begin_node(w, n, "ENUM_VARIANT"); emit_str(w, "name", v->name); emit_list(w, "fields", v->fields); jw_end_object(w); break;
        }
        case AST_POINTER_TYPE: {
            begin_node(w, n, "POINTER_TYPE"); emit_child(w, "element_type", ((PointerTypeNode*)n)->element_type); jw_end_object(w); break;
        }
        case AST_REFERENCE_TYPE: {
            ReferenceTypeNode* t = (ReferenceTypeNode*)n;
            begin_node(w, n, "REFERENCE_TYPE"); emit_child(w, "element_type", t->element_type); emit_bool(w, "is_mutable", t->is_mutable); jw_end_object(w); break;
        }
        case AST_UNSAFE_PTR_TYPE: {
            begin_node(w, n, "UNSAFE_PTR_TYPE"); emit_child(w, "element_type", ((UnsafePtrTypeNode*)n)->element_type); jw_end_object(w); break;
        }
        case AST_ERROR_UNION_TYPE: {
            ErrorUnionTypeNode* t = (ErrorUnionTypeNode*)n;
            begin_node(w, n, "ERROR_UNION_TYPE"); emit_child(w, "value_type", t->value_type); emit_child(w, "error_type", t->error_type); jw_end_object(w); break;
        }
        case AST_NULLABLE_TYPE: {
            begin_node(w, n, "NULLABLE_TYPE"); emit_child(w, "base_type", ((NullableTypeNode*)n)->base_type); jw_end_object(w); break;
        }
```

- [ ] **Step 4: Run the parse-stage probe to green**

Run: `make lexer && bash scripts/program_dump_probe.sh 2>&1 | tail -4`
Expected: `--emit-ast-json: 647 fixtures, 0 failed` (495 run + 152 non-parse-error rejects), then the typed rows fail because `--emit-program` is not wired yet. If a fixture names another unsupported kind, add that case following the same shape; the kinds above are the ones the current grammar produces, and the abort message names any this list missed.

- [ ] **Step 5: Commit**

```bash
git add src/ast/program_dump.c
git commit -m "feat(dump): expressions and type expressions; parse stage covers every fixture"
```

---

### Task 5: Typed stage — type table, plan section, driver hook

**Files:**
- Modify: `src/ast/program_dump.c` (replace the two placeholders)
- Modify: `src/compiler/goo.c:1468` (typed-stage hook)

- [ ] **Step 1: Write the type table emitter**

Replace the `emit_type_entry` placeholder:

```c
static const char* type_kind_name(TypeKind k) {
    switch (k) {
        case TYPE_VOID: return "VOID"; case TYPE_BOOL: return "BOOL";
        case TYPE_INT8: return "INT8"; case TYPE_INT16: return "INT16"; case TYPE_INT32: return "INT32"; case TYPE_INT64: return "INT64";
        case TYPE_UINT8: return "UINT8"; case TYPE_UINT16: return "UINT16"; case TYPE_UINT32: return "UINT32"; case TYPE_UINT64: return "UINT64";
        case TYPE_FLOAT32: return "FLOAT32"; case TYPE_FLOAT64: return "FLOAT64";
        case TYPE_STRING: return "STRING"; case TYPE_CHAR: return "CHAR";
        case TYPE_ARRAY: return "ARRAY"; case TYPE_SLICE: return "SLICE"; case TYPE_MAP: return "MAP"; case TYPE_CHANNEL: return "CHANNEL";
        case TYPE_FUNCTION: return "FUNCTION"; case TYPE_POINTER: return "POINTER"; case TYPE_REFERENCE: return "REFERENCE";
        case TYPE_STRUCT: return "STRUCT"; case TYPE_ENUM: return "ENUM"; case TYPE_INTERFACE: return "INTERFACE";
        case TYPE_ERROR_UNION: return "ERROR_UNION"; case TYPE_NULLABLE: return "NULLABLE"; case TYPE_QUALIFIED: return "QUALIFIED";
        case TYPE_PARAM: return "PARAM";
        default: die_kind("type", k); return NULL;   // CONCEPT / HKT / CONSTRUCTOR / APPLICATION are attic-era
    }
}

static void emit_type_ref(JsonW* w, const char* key, Type* t) { jw_key(w, key); if (t) jw_int(w, type_id(t)); else jw_null(w); }

static void emit_type_entry(JsonW* w, size_t id, Type* t) {
    jw_begin_object(w);
    emit_int(w, "id", (long long)id);
    emit_str(w, "kind", type_kind_name(t->kind));
    emit_str(w, "name", t->name);
    emit_int(w, "size", (long long)t->size);
    emit_int(w, "align", (long long)t->align);
    switch (t->kind) {
        case TYPE_ARRAY:
            emit_type_ref(w, "element", t->data.array.element_type);
            emit_int(w, "length", (long long)t->data.array.length);
            emit_bool(w, "comptime_length", t->data.array.comptime_length); break;
        case TYPE_SLICE: emit_type_ref(w, "element", t->data.slice.element_type); break;
        case TYPE_MAP: emit_type_ref(w, "key", t->data.map.key_type); emit_type_ref(w, "value", t->data.map.value_type); break;
        case TYPE_CHANNEL:
            emit_type_ref(w, "element", t->data.channel.element_type);
            emit_str(w, "pattern", chan_pattern_name(t->data.channel.pattern));
            emit_str(w, "endpoint", t->data.channel.endpoint); break;
        case TYPE_FUNCTION:
            jw_key(w, "params"); jw_begin_array(w);
            for (size_t i = 0; i < t->data.function.param_count; i++) jw_int(w, type_id(t->data.function.param_types[i]));
            jw_end_array(w);
            emit_type_ref(w, "return", t->data.function.return_type);
            emit_bool(w, "is_variadic", t->data.function.is_variadic);
            emit_bool(w, "has_comptime_params", t->data.function.has_comptime_params); break;
        case TYPE_POINTER: emit_type_ref(w, "pointee", t->data.pointer.pointee_type); break;
        case TYPE_REFERENCE: emit_type_ref(w, "referenced", t->data.reference.referenced_type); emit_bool(w, "is_mutable", t->data.reference.is_mutable); break;
        case TYPE_STRUCT:
            emit_str(w, "struct_name", t->data.struct_type.name);
            jw_key(w, "fields"); jw_begin_array(w);
            for (size_t i = 0; i < t->data.struct_type.field_count; i++) {
                StructField* f = &t->data.struct_type.fields[i];
                jw_begin_object(w);
                emit_str(w, "name", f->name); emit_type_ref(w, "type", f->type);
                emit_int(w, "offset", (long long)f->offset); emit_str(w, "ownership", ownership_name(f->ownership));
                jw_end_object(w);
            }
            jw_end_array(w); break;
        case TYPE_ENUM:
            emit_str(w, "enum_name", t->data.enum_type.name);
            jw_key(w, "variants"); jw_begin_array(w);
            for (size_t i = 0; i < t->data.enum_type.variant_count; i++) {
                EnumVariant* v = &t->data.enum_type.variants[i];
                jw_begin_object(w); emit_str(w, "name", v->name); emit_type_ref(w, "payload", v->payload); emit_int(w, "tag", v->tag); jw_end_object(w);
            }
            jw_end_array(w); break;
        case TYPE_INTERFACE:
            emit_str(w, "interface_name", t->data.interface.name);
            emit_bool(w, "is_synthesized", t->data.interface.is_synthesized);
            jw_key(w, "methods"); jw_begin_array(w);
            for (InterfaceMethod* m = t->data.interface.methods; m; m = m->next) {
                jw_begin_object(w); emit_str(w, "name", m->name); emit_type_ref(w, "type", m->type); jw_end_object(w);
            }
            jw_end_array(w); break;
        case TYPE_ERROR_UNION: emit_type_ref(w, "value", t->data.error_union.value_type); emit_type_ref(w, "error", t->data.error_union.error_type); break;
        case TYPE_NULLABLE: emit_type_ref(w, "base", t->data.nullable.base_type); break;
        case TYPE_QUALIFIED: emit_type_ref(w, "base", t->data.qualified.base_type); emit_str(w, "ownership", ownership_name(t->data.qualified.ownership)); break;
        case TYPE_PARAM: emit_str(w, "param_name", t->data.type_param.name); emit_int(w, "index", t->data.type_param.index); emit_type_ref(w, "constraint", t->data.type_param.constraint); break;
        default: break;   // scalar kinds carry no fields
    }
    jw_end_object(w);
}
```

Note: `interface.methods` is a linked list per `types.h:300-304`, and `struct_type.fields` / `enum_type.variants` are arrays per `:280-297`. If the compiler reports a different layout, follow the header, not this plan.

- [ ] **Step 2: Write the plan emitter**

Replace the `emit_plan` placeholder:

```c
static void emit_pos_list(JsonW* w, const char* key, ASTNode** nodes, size_t count) {
    jw_key(w, key); jw_begin_array(w);
    for (size_t i = 0; i < count; i++) {
        jw_begin_array(w); jw_int(w, nodes[i]->pos.line); jw_int(w, nodes[i]->pos.column); jw_int(w, nodes[i]->pos.offset); jw_end_array(w);
    }
    jw_end_array(w);
}

static void emit_plan(JsonW* w, ReleasePlan* plan) {
    jw_begin_array(w);
    for (size_t i = 0; i < plan->count; i++) {
        ReleasePlanFunction* f = &plan->functions[i];
        jw_begin_object(w);
        emit_str(w, "function", f->function_name);
        jw_key(w, "locals"); jw_begin_array(w);
        for (size_t j = 0; j < f->count; j++) {
            ReleaseDecision* d = &f->decisions[j];
            char buf[ESCAPE_REASON_NAMES_MAX];
            jw_begin_object(w);
            emit_str(w, "name", d->local_name);
            emit_str(w, "verdict", release_verdict_name(d->verdict));
            emit_str(w, "reasons", escape_reason_names(d->diagnostic_reasons, buf, sizeof buf));
            emit_bool(w, "owns_elems", d->owns_elems);
            jw_end_object(w);
        }
        jw_end_array(w);
        emit_pos_list(w, "owned_keys", f->owned_keys, f->owned_key_count);
        emit_pos_list(w, "owned_concat_operands", f->owned_concat_operands, f->owned_concat_count);
        jw_end_object(w);
    }
    jw_end_array(w);
}
```

- [ ] **Step 3: Wire the typed-stage flag into the driver**

`src/compiler/goo.c`, immediately after the `type_check_program_files` success check at `:1468` (i.e. once checking has passed and before `Phase 4: Code generation`):

```c
    if (options->emit_program) {
        // Same plan the codegen would build (codegen.c:376), built here so the
        // dump needs no LLVM and no output file. GOO_ARC_RELEASE=0 yields NULL
        // plans, emitted as null — the kill switch is visible in the dump.
        ReleasePlan** plans = calloc(nfiles, sizeof(ReleasePlan*));
        const char* arc_off = getenv("GOO_ARC_RELEASE");
        if (!(arc_off && strcmp(arc_off, "0") == 0)) {
            for (size_t fi = 0; fi < nfiles; fi++) plans[fi] = release_plan_analyze(asts[fi]);
        }
        program_dump_write(stdout, asts, (const char**)filenames, nfiles, plans, PROGRAM_DUMP_TYPED);
        for (size_t fi = 0; fi < nfiles; fi++) if (plans[fi]) release_plan_free(plans[fi]);
        free(plans);
        type_checker_free(type_checker);
        ENTRY_CLEANUP();
        return true;
    }
```
Add `#include "release_decision.h"` if `goo.c` does not already include it.

- [ ] **Step 4: Run the probe to green**

Run: `make lexer && bash scripts/program_dump_probe.sh 2>&1 | tail -3`
Expected:
```
  --emit-ast-json: 647 fixtures, 0 failed
  --emit-program: 495 fixtures, 0 failed
program-dump-probe: PASS
```
If a typed row fails with `typed stage but no type id` on some expression kind, that is a real finding: the checker pass leaves that node unstamped and codegen re-checks it later. Record the fixture and kind in the commit message and add the kind to a `known_unstamped` allowlist in `program_dump_check.py` with a comment naming the fixture; closing that gap is Phase 1 work, not this task's.

- [ ] **Step 5: Run the whole net**

Run: `make -k verify-core 2>&1 | tail -3`
Expected: `verify-core: ALL GREEN GATES PASSED (ccomp-free)` (on the Linux container; the macOS-only failures documented in memory are not regressions).

- [ ] **Step 6: Commit**

```bash
git add src/ast/program_dump.c src/compiler/goo.c
git commit -m "feat(dump): --emit-program: typed stage with type table and release plan"
```

---

### Task 6: Differential harness with teeth

**Files:**
- Create: `scripts/frontend_diff.sh`
- Modify: `Makefile` (two targets, gate entry)

- [ ] **Step 1: Write the harness**

```bash
#!/usr/bin/env bash
# frontend_diff.sh — run two producers over the same fixtures and diff their
# dumps. A producer is a command that takes one fixture path and writes a dump
# to stdout. Reports "N same, M differ" and lists the differing fixtures;
# exit 1 if any differ. This is the gate every front-end swap is measured by:
# when producer B is the Haskell front end and M is 0 across every fixture,
# that component may flip.
#
#   scripts/frontend_diff.sh --a 'CMD' --b 'CMD' [--list FILE]
#   scripts/frontend_diff.sh --self-test
#
# Teeth: --self-test proves it reports a difference (b = sed rewrite) and
# reports none when the producers agree (a = b = cat).
set -u
cd "$(dirname "$0")/.."

self_test() {
    local tmp; tmp=$(mktemp -d)
    printf 'package main\n' > "$tmp/f1.goo"; printf 'package other\n' > "$tmp/f2.goo"
    printf '%s\n%s\n' "$tmp/f1.goo" "$tmp/f2.goo" > "$tmp/list"
    if ! out=$(bash "$0" --a cat --b cat --list "$tmp/list"); then echo "frontend-diff --self-test: FAIL (identical producers reported a difference)"; exit 1; fi
    if out=$(bash "$0" --a cat --b "sed s/other/changed/" --list "$tmp/list"); then echo "frontend-diff --self-test: FAIL (a rewritten fixture was not reported)"; exit 1; fi
    echo "$out" | grep -q 'f2.goo' || { echo "frontend-diff --self-test: FAIL (the differing fixture was not named)"; exit 1; }
    rm -rf "$tmp"
    echo "frontend-diff --self-test: PASS (2 same reports 0; 1 rewrite reports 1 and names it)"
}

A=""; B=""; LIST=""
while [ $# -gt 0 ]; do
    case "$1" in
        --self-test) self_test; exit 0 ;;
        --a) A=$2; shift 2 ;;
        --b) B=$2; shift 2 ;;
        --list) LIST=$2; shift 2 ;;
        *) echo "unknown argument: $1"; exit 2 ;;
    esac
done
[ -n "$A" ] && [ -n "$B" ] || { echo "usage: $0 --a CMD --b CMD [--list FILE]"; exit 2; }

fixtures() {
    if [ -n "$LIST" ]; then cat "$LIST"; else ls examples/*.expected.txt | sed 's/\.expected\.txt$/.goo/'; fi
}

tmp=$(mktemp -d); same=0; differ=0
while IFS= read -r f; do
    [ -z "$f" ] && continue
    $A "$f" > "$tmp/a" 2>/dev/null; $B "$f" > "$tmp/b" 2>/dev/null
    if cmp -s "$tmp/a" "$tmp/b"; then same=$((same+1)); else differ=$((differ+1)); echo "  DIFF  $f"; fi
done < <(fixtures)
rm -rf "$tmp"
echo "frontend-diff: $same same, $differ differ"
[ "$differ" -eq 0 ]
```

- [ ] **Step 2: Run the self-test and see it pass**

Run: `chmod +x scripts/frontend_diff.sh && bash scripts/frontend_diff.sh --self-test`
Expected: `frontend-diff --self-test: PASS (...)`.

- [ ] **Step 3: Prove the harness against the real dump**

Run: `bash scripts/frontend_diff.sh --a './bin/goo --emit-ast-json -o /dev/null' --b './bin/goo --emit-ast-json -o /dev/null' | tail -1`
Expected: `frontend-diff: 495 same, 0 differ`.

- [ ] **Step 4: Makefile**

After the `program-dump-selftest` stanza:
```make
# The harness itself has teeth: identical producers report 0, one rewritten
# fixture reports 1 and is named. Without this a broken harness could report
# "0 differ" between a real front end and an empty one.
frontend-diff-selftest:
	@bash scripts/frontend_diff.sh --self-test
```
Add `frontend-diff-selftest \` to `VERIFY_ALL_DEPS` before `archive-determinism-probe \`.

- [ ] **Step 5: Commit**

```bash
git add scripts/frontend_diff.sh Makefile
git commit -m "feat(gates): frontend_diff harness with self-test"
```

---

### Task 7: Diagnostic catalogue and drift gate

**Files:**
- Create: `scripts/extract_diagnostics.py`
- Create: `catalogue/diagnostics.tsv`
- Create: `scripts/diagnostics_drift_probe.sh`
- Modify: `Makefile`

- [ ] **Step 1: Write the extractor**

```python
#!/usr/bin/env python3
"""Regenerate catalogue/diagnostics.tsv from the checker's type_error call
sites (src/types/*.c). type_error_union is a TYPE CONSTRUCTOR, not an
emitter, and is deliberately not matched. The format string is the third
argument (types.h:1338: type_error(checker, pos, format, ...)). Adjacent
string literals are joined the way the C compiler joins them.

Columns: id  file  format
The id is stable across edits: sha1 of (file basename, format) truncated to
8 hex chars, so a moved call keeps its id and a reworded one gets a new one.
Sorted by file then format so the file is diff-friendly."""
import glob, hashlib, re, sys

CALL = re.compile(r'\btype_error(?:_union)?\s*\(', re.M)
STR = re.compile(r'"((?:[^"\\]|\\.)*)"')

def formats_in(src):
    out = []
    for m in CALL.finditer(src):
        i = m.end()
        # skip two arguments (checker, pos) by counting top-level commas
        depth, args = 0, 0
        while i < len(src) and args < 2:
            c = src[i]
            if c in '([': depth += 1
            elif c in ')]': depth -= 1
            elif c == ',' and depth == 0: args += 1
            i += 1
        # collect adjacent string literals
        parts = []
        while True:
            ws = re.match(r'\s*', src[i:]); i += ws.end()
            s = STR.match(src, i)
            if not s: break
            parts.append(s.group(1)); i = s.end()
        if parts: out.append(''.join(parts))
    return out

def main():
    rows = []
    for path in sorted(glob.glob('src/types/*.c')):
        base = path.split('/')[-1]
        for fmt in formats_in(open(path).read()):
            ident = hashlib.sha1(f'{base}\0{fmt}'.encode()).hexdigest()[:8]
            rows.append((ident, base, fmt))
    rows = sorted(set(rows), key=lambda r: (r[1], r[2]))
    sys.stdout.write('id\tfile\tformat\n')
    for r in rows: sys.stdout.write('\t'.join(r) + '\n')

if __name__ == '__main__': main()
```

- [ ] **Step 2: Generate the catalogue and check its size**

Run: `mkdir -p catalogue && python3 scripts/extract_diagnostics.py > catalogue/diagnostics.tsv && wc -l catalogue/diagnostics.tsv`
Expected: 280 lines (304 `type_error(` sites; duplicates collapse, and two ternary sites contribute two rows each). `type_error_union(` is a type constructor and is not a diagnostic. If the count is under 250, the argument-skipping loop is wrong for some call shape: open the first file and compare the call it missed against the regex.

- [ ] **Step 3: Write the drift probe with teeth**

```bash
#!/usr/bin/env bash
# diagnostics-drift-probe — catalogue/diagnostics.tsv must equal what
# scripts/extract_diagnostics.py generates from the tree. The same shape as
# stdlib_coverage_drift.sh: a committed table that quietly falls behind the
# source is worse than no table.
# Teeth: --self-test appends a fake row to a copy and expects a diff.
set -u
cd "$(dirname "$0")/.."
if [ "${1:-}" = "--self-test" ]; then
    tmp=$(mktemp)
    python3 scripts/extract_diagnostics.py > "$tmp"
    printf 'deadbeef\tfake.c\tthis row does not exist\n' >> "$tmp"
    if diff -q "$tmp" <(python3 scripts/extract_diagnostics.py) >/dev/null; then echo "diagnostics-drift-probe --self-test: FAIL (an extra row went unnoticed)"; exit 1; fi
    rm -f "$tmp"
    echo "diagnostics-drift-probe --self-test: PASS (an extra row is a diff)"; exit 0
fi
if diff -u catalogue/diagnostics.tsv <(python3 scripts/extract_diagnostics.py) > /tmp/diag_drift.$$; then
    echo "diagnostics-drift-probe: PASS ($(($(wc -l < catalogue/diagnostics.tsv) - 1)) diagnostics, catalogue matches the tree)"; rm -f /tmp/diag_drift.$$
else
    echo "diagnostics-drift-probe: FAIL (catalogue/diagnostics.tsv is stale; regenerate with scripts/extract_diagnostics.py)"; head -20 /tmp/diag_drift.$$; rm -f /tmp/diag_drift.$$; exit 1
fi
```

- [ ] **Step 4: Run both**

Run: `chmod +x scripts/diagnostics_drift_probe.sh && bash scripts/diagnostics_drift_probe.sh --self-test && bash scripts/diagnostics_drift_probe.sh`
Expected: both PASS lines.

- [ ] **Step 5: Makefile**

```make
# The type checker's diagnostic strings, as a table the Haskell front end
# reads. Reject fixtures match stderr substrings, so verbatim text is the
# parity contract; this gate keeps the table equal to the source.
diagnostics-drift-probe:
	@bash scripts/diagnostics_drift_probe.sh

diagnostics-drift-selftest:
	@bash scripts/diagnostics_drift_probe.sh --self-test
```
Add both to `VERIFY_ALL_DEPS` before `archive-determinism-probe \`.

- [ ] **Step 6: Run the net and commit**

Run: `make -k verify-core 2>&1 | tail -2`
Expected: ALL GREEN.

```bash
git add scripts/extract_diagnostics.py catalogue/diagnostics.tsv scripts/diagnostics_drift_probe.sh Makefile
git commit -m "feat(gates): diagnostic catalogue extracted from the checker, with drift gate"
```

---

### Task 8: Format specification and gate-count baseline

**Files:**
- Create: `docs/superpowers/specs/2026-09-04-program-dump-format.md`
- Modify: `tools/parity-gate-count.txt` (the Bazel parity baseline counts `VERIFY_ALL_DEPS`; it moves from 217 to 224)
- Modify: `CLAUDE.md` (Build System section: the two flags and three gates)

- [ ] **Step 1: Write the spec from the emitters**

The document has one table per node family listing kind name, fields, and which are lists/children/scalars, taken verbatim from the `case` arms in `src/ast/program_dump.c`, plus the type-table and plan sections from Task 5 and the envelope from the top of this plan. It states the three invariants: key order is emission order; `types[i].id == i` and the table is closed under reference; every unknown kind aborts by name.

- [ ] **Step 2: Bump the parity baseline**

Run: `grep -c '' tools/parity-gate-count.txt; sed -n '/^VERIFY_ALL_DEPS :=/,/[^\\]$/p' Makefile | sed 's/VERIFY_ALL_DEPS :=//; s/\\//g' | tr ' \t' '\n\n' | grep -cE '^[a-z]'`
Expected: the second number is the new count; write it into `tools/parity-gate-count.txt` (format per that file).

- [ ] **Step 3: CLAUDE.md**

Under "Build System" add:
```
- `bin/goo --emit-ast-json <file>` / `--emit-program <file>` — the program dump
  (docs/superpowers/specs/2026-09-04-program-dump-format.md). Gated by
  `program-dump-probe`; `scripts/frontend_diff.sh` diffs two producers of it.
  `catalogue/diagnostics.tsv` is the checker's message table, gated by
  `diagnostics-drift-probe`.
```

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/specs/2026-09-04-program-dump-format.md tools/parity-gate-count.txt CLAUDE.md
git commit -m "docs(dump): program dump format spec; gate-count baseline; CLAUDE.md"
```

---

## Self-review

- **Spec coverage:** parent tasks 2 (schema → Task 8 written from the emitters, validated by Task 2's checker), 3 (serializer → Tasks 2–5), 4 (harness → Task 6), 5 (catalogue → Task 7). Task 1 of the parent is deferred to the Haskell lexer plan, stated in the header.
- **Placeholders:** the two `abort()` placeholders in Task 2 are replaced in Task 5 and are unreachable before it (the typed flag does not exist until Task 5). The `filenames` array name in Task 2 Step 5 is the one hedged reference; the step says how to find it.
- **Type consistency:** `program_dump_write(FILE*, ASTNode**, const char**, size_t, ReleasePlan**, ProgramDumpStage)` is used identically in Tasks 2 and 5; `emit_child/emit_list/emit_str/emit_int/emit_bool/emit_tok/emit_type_ref` signatures match across Tasks 2–5; probe flag names `--emit-ast-json`/`--emit-program` match the driver and the scripts; gate names in the Makefile match `VERIFY_ALL_DEPS` entries.
