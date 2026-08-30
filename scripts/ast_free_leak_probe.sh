#!/bin/bash
# ast-free-leak probe: ast_node_free() frees the whole tree, not only the spine.
#
# WHY THIS EXISTS. The parser fuzzer found on 2026-08-08 that every assignment
# statement leaked 188 bytes, linear in the count. ast_node_free() had no
# `case AST_EXPR_STMT`, so the node reached `default:` and nothing freed its
# `expr` child.
#
# The leak was invisible to every existing gate, and that is the point of this
# one. The compiler is a batch process: it exits without freeing, so a leak in
# ast_node_free costs it nothing and no probe on bin/goo can see it. Only a
# caller that parses AND frees in one process can. This runs that caller under
# valgrind.
#
# WHAT IS ASSERTED. Zero "definitely lost" bytes across 50 parse-and-free
# cycles. Not a threshold -- an exact zero. A per-statement leak scales with the
# iteration count, so a threshold would only record how big this fixture is.
#
# TEETH. --self-test removes the fix from a SCRATCH copy of src/ast/ast.c and
# requires the probe to fail. A leak probe that has only ever passed has not
# shown it can report a leak, and this one was written against a KNOWN leak, so
# the RED state is reproducible on demand rather than hypothetical.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

CC_="${CC_PROBE:-gcc}"
CSTD="${CSTD_PROBE:--std=c23}"
AST_SRC="${AST_SRC:-src/ast/ast.c}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

SELF_TEST=0
[ "${1:-}" = "--self-test" ] && SELF_TEST=1

# Every source the driver needs: lexer, parser, ast, errors. No type checker,
# no codegen, no LLVM -- the same minimal set the fuzz harness links.
SRCS="tests/unit/ast/ast_free_leak_test.c
src/lexer/lexer.c src/lexer/token.c
src/parser/parser.tab.c src/parser/lexer_bridge.c
src/parser/parser_errors.c src/parser/parser_actions.c
src/parser/parser_state.c
src/ast/ast_constructors.c
src/errors/error.c src/errors/ergonomic_errors.c"

CFLAGS_="-g -O0 -I. -Iinclude -D_GNU_SOURCE -include include/xalloc.h -include include/goo_assert.h -DLLVM_AVAILABLE=0"

# Build with a given ast.c (the tracked one, or a mutant). Prints the binary path.
build_with() {  # $1 = path to the ast.c to use
    local astc="$1" out="$2"
    # shellcheck disable=SC2086
    $CC_ $CSTD $CFLAGS_ $SRCS "$astc" -o "$out" -lm 2>"$WORK/cc.err"
}

# Runs the driver under valgrind. Echoes the "definitely lost" byte count.
# Echoes -1 when the driver itself refused to run (its own instrument check).
measure() {  # $1 = binary
    local bin="$1"
    valgrind --leak-check=full --error-exitcode=0 "$bin" \
        >"$WORK/run.out" 2>"$WORK/vg.log"
    local rc=$?
    if [ "$rc" -ne 0 ]; then echo "-1"; return; fi
    # "definitely lost: 1,234 bytes in 5 blocks" -> 1234
    sed -n 's/.*definitely lost: \([0-9,]*\) bytes.*/\1/p' "$WORK/vg.log" \
        | tr -d ',' | head -1 | grep -E '^[0-9]+$' || echo 0
}

if ! command -v valgrind >/dev/null 2>&1; then
    echo "ast-free-leak-probe: SKIPPED (valgrind not installed)"
    exit 0
fi

echo "=== ast-free-leak-probe: ast_node_free frees the whole tree ==="

if [ "$SELF_TEST" -eq 1 ]; then
    # ---------------------------------------------------------------------
    # Teeth. Delete the AST_EXPR_STMT case from a SCRATCH copy and require the
    # probe to report a leak. The tracked file is never written -- checksummed
    # before and after, following scripts/release_decision_teeth.sh.
    # ---------------------------------------------------------------------
    before="$(sha256sum "$AST_SRC" | cut -d' ' -f1)"

    python3 - "$AST_SRC" "$WORK/ast_mutant.c" <<'PY'
import re, sys
src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()
# Remove the whole `case AST_EXPR_STMT: { ... }` block.
pat = re.compile(r"\n[ \t]*case AST_EXPR_STMT: \{.*?\n[ \t]*\}\n", re.S)
new, n = pat.subn("\n", s, count=1)
if n != 1:
    sys.stderr.write(f"MUTATION FAILED: matched {n} AST_EXPR_STMT blocks, expected 1\n")
    sys.exit(3)
open(dst, "w").write(new)
PY
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "  SELF-TEST BROKEN — could not remove the AST_EXPR_STMT case."
        echo "  Either the fix is absent, or its shape changed and this mutation"
        echo "  no longer finds it. Either way the teeth below prove nothing."
        exit 1
    fi

    after="$(sha256sum "$AST_SRC" | cut -d' ' -f1)"
    [ "$before" = "$after" ] || { echo "  SELF-TEST BROKEN — tracked $AST_SRC was modified"; exit 1; }

    echo "--- Control 1: tracked ast.c -> 0 bytes lost ---"
    build_with "$AST_SRC" "$WORK/p_ok" || { echo "  build FAILED"; sed 's/^/    /' "$WORK/cc.err"; exit 1; }
    ok="$(measure "$WORK/p_ok")"
    echo "  got: $ok bytes"

    echo "--- Control 2: AST_EXPR_STMT case removed -> MUST leak ---"
    build_with "$WORK/ast_mutant.c" "$WORK/p_bad" || { echo "  build FAILED"; sed 's/^/    /' "$WORK/cc.err"; exit 1; }
    bad="$(measure "$WORK/p_bad")"
    echo "  got: $bad bytes"

    rc=0
    [ "$ok" = "0" ]   || { echo "  CONTROL 1 FAILED — the tracked build leaks $ok bytes"; rc=1; }
    [ "$bad" -gt 0 ] 2>/dev/null || { echo "  CONTROL 2 FAILED — removing the fix leaked nothing, so this probe cannot see the defect it guards"; rc=1; }

    echo
    if [ "$rc" -eq 0 ]; then
        echo "ast-free-leak-probe --self-test: TEETH CONFIRMED (clean=0, mutant=$bad)"
    else
        echo "ast-free-leak-probe --self-test: TEETH MISSING"
    fi
    exit "$rc"
fi

# -------------------------------------------------------------------------
# Ordinary run.
# -------------------------------------------------------------------------
build_with "$AST_SRC" "$WORK/p" || {
    echo "ast-free-leak-probe: FAIL — driver did not build"
    sed 's/^/    /' "$WORK/cc.err"
    exit 1
}

lost="$(measure "$WORK/p")"

if [ "$lost" = "-1" ]; then
    echo "ast-free-leak-probe: FAIL — the driver refused to run:"
    sed 's/^/    /' "$WORK/run.out"
    exit 1
fi

sed 's/^/  /' "$WORK/run.out"

# Real memory errors, not just leaks. A free-list change is exactly the class
# that produced the PR #278 use-after-free, so this probe refuses to report a
# clean leak count while an invalid access sits in the same log.
if grep -qE "Invalid read|Invalid write|Invalid free|Mismatched free|double free" "$WORK/vg.log"; then
    echo "  MEMORY ERROR in the valgrind log:"
    grep -E "Invalid read|Invalid write|Invalid free|Mismatched free|double free" "$WORK/vg.log" | head -5 | sed 's/^/    /'
    echo "ast-free-leak-probe: FAIL — freeing the tree caused an invalid access"
    exit 1
fi

if [ "$lost" != "0" ]; then
    echo "  definitely lost: $lost bytes"
    grep -A6 "definitely lost" "$WORK/vg.log" | head -12 | sed 's/^/    /'
    echo "ast-free-leak-probe: FAIL — ast_node_free left $lost bytes behind"
    exit 1
fi

echo "  definitely lost: 0 bytes"
echo "  no invalid read, write, free or double free"
echo "ast-free-leak-probe: PASS"
