#!/bin/bash
# goo-assert probe: the three build modes of include/goo_assert.h.
#
# WHY THIS EXISTS. GOO_ASSERT, GOO_NEVER and GOO_ALWAYS are force-included into
# every translation unit and expand to DIFFERENT code in three builds. That is
# the whole point of them, and it is also the whole risk: a production build
# where GOO_ASSERT still aborts would ship a compiler that dies on a
# recoverable condition, and a debug build where it silently does nothing is a
# macro that looks like a check and is not one.
#
# Neither failure is visible by reading the header, because the preprocessor
# picks the arm. So compile the same fixture three ways and observe it.
#
# THE ASSERTIONS, one per build:
#
#   production (no flag)  GOO_ASSERT(0) must NOT abort  -> exit 0
#                         GOO_NEVER(x)/GOO_ALWAYS(x) pass x through, so a
#                         defensive branch is still THERE as a last defence.
#   debug (GOO_DEBUG)     GOO_ASSERT(0) MUST abort      -> SIGABRT
#                         GOO_NEVER(1) MUST abort, GOO_ALWAYS(0) MUST abort.
#   coverage (GOO_COVERAGE)
#                         GOO_NEVER -> constant 0, GOO_ALWAYS -> constant 1,
#                         regardless of the argument. This is what keeps an
#                         unreachable defensive branch out of the coverage
#                         denominator.
#
# The production and coverage rows are the ones that would otherwise rot in
# silence: nothing else in the tree would notice if they stopped holding.

set -u

# A Bazel sh_test starts with $PWD already AT the runfiles root, but $0
# resolves to <runfiles>/_main/tests/probes/goo_assert_probe.sh -- a symlink
# one directory too deep for the old dirname-based fallback to find its way
# back to the root (measured: -I"$ROOT/include" reported "goo_assert.h: No
# such file or directory"). git rev-parse fails in the sandbox (no .git), so
# the fallback is $PWD itself, which IS the runfiles root there and the repo
# root here.
ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"

# Use the compiler the MAKEFILE uses, not a bare `gcc`. CI runs
# `make CC=gcc-14 ...` because the distro default on ubuntu-24.04 is gcc-13,
# which rejects -std=c23 (.github/workflows/tests.yml:72). A hardcoded `gcc`
# here compiled locally and failed on CI with "unrecognized command-line option
# '-std=c23'" -- and the probe correctly reported FAIL rather than passing, so
# this comment exists because the probe worked, not because it did not.
#
# The Makefile passes CC_PROBE="$(CC)", which is "$(CCACHE) gcc" locally: two
# words. It is deliberately left unquoted at the call sites below so the shell
# splits it. CFLAGS_PROBE carries -std=..., for the same reason.
CC_="${CC_PROBE:-gcc}"
CSTD="${CSTD_PROBE:--std=c23}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

rc=0
pass() { echo "  PASS $1"; }
bad()  { echo "  FAIL $1"; rc=1; }

cat > "$WORK/probe.c" <<'EOF'
#include <stdio.h>
#include "goo_assert.h"

int main(void) {
    // Opaque to the optimiser, so the compiler cannot fold these into the
    // constants the coverage build is supposed to produce by ITSELF. Without
    // this, the coverage row would pass even if GOO_COVERAGE did nothing.
    volatile int one = 1;
    volatile int zero = 0;

    printf("NEVER=%d ALWAYS=%d\n", GOO_NEVER(one), GOO_ALWAYS(zero));
    GOO_ASSERT(zero);          // false: aborts in debug, no-op elsewhere
    printf("survived\n");
    return 0;
}
EOF

build_and_run() {  # $1 = extra flags, $2 = output var prefix
    $CC_ $CSTD -I"$ROOT/include" $1 "$WORK/probe.c" -o "$WORK/p" 2>"$WORK/cc.err" || {
        echo "  compile FAILED with flags '$1':"; sed 's/^/    /' "$WORK/cc.err"; return 99
    }
    "$WORK/p" > "$WORK/out" 2>/dev/null
    echo $?
}

echo "=== goo-assert-probe: three build modes of include/goo_assert.h ==="

# ---------------------------------------------------------------------------
echo "--- production (no GOO_DEBUG, no GOO_COVERAGE) ---"
st=$(build_and_run "")
out="$(cat "$WORK/out" 2>/dev/null)"
if [ "$st" = "0" ] && grep -q "survived" "$WORK/out" 2>/dev/null; then
    pass "GOO_ASSERT(0) does not abort (exit 0, reached 'survived')"
else
    bad "production build aborted or exited $st — GOO_ASSERT is not compiled out"
fi
# Pass-through is the property that keeps the defensive branch in the shipped
# binary. NEVER(1) must be 1 here, NOT the coverage build's constant 0.
if [ "$out" != "${out#NEVER=1 ALWAYS=0}" ]; then
    pass "GOO_NEVER/GOO_ALWAYS pass the condition through (NEVER=1 ALWAYS=0)"
else
    bad "expected 'NEVER=1 ALWAYS=0' in production, got: $(head -1 "$WORK/out" 2>/dev/null)"
fi

# ---------------------------------------------------------------------------
echo "--- debug (-DGOO_DEBUG) ---"
st=$(build_and_run "-DGOO_DEBUG")
# 134 = 128 + SIGABRT(6). This is the row that proves an assert is a check and
# not a decoration.
if [ "$st" = "134" ]; then
    pass "GOO_ASSERT(0) aborts (SIGABRT, rc 134)"
else
    bad "debug build exited $st, expected 134 (SIGABRT) — the assert did not fire"
fi

cat > "$WORK/never.c" <<'EOF'
#include "goo_assert.h"
int main(void) { volatile int one = 1; return GOO_NEVER(one); }
EOF
$CC_ $CSTD -I"$ROOT/include" -DGOO_DEBUG "$WORK/never.c" -o "$WORK/n" 2>/dev/null
# Status taken inside a command substitution: a bare invocation makes the shell
# print its own "Aborted (core dumped)" job-control line, which looks like probe
# output and is not.
st=$( "$WORK/n" >/dev/null 2>&1; echo $? )
[ "$st" = "134" ] && pass "GOO_NEVER(true) aborts" || bad "GOO_NEVER(true) exited $st, expected 134"

cat > "$WORK/always.c" <<'EOF'
#include "goo_assert.h"
int main(void) { volatile int zero = 0; return GOO_ALWAYS(zero); }
EOF
$CC_ $CSTD -I"$ROOT/include" -DGOO_DEBUG "$WORK/always.c" -o "$WORK/a" 2>/dev/null
st=$( "$WORK/a" >/dev/null 2>&1; echo $? )
[ "$st" = "134" ] && pass "GOO_ALWAYS(false) aborts" || bad "GOO_ALWAYS(false) exited $st, expected 134"

# ---------------------------------------------------------------------------
echo "--- coverage (-DGOO_COVERAGE) ---"
st=$(build_and_run "-DGOO_COVERAGE")
out="$(head -1 "$WORK/out" 2>/dev/null)"
if [ "$st" = "0" ] && [ "$out" = "NEVER=0 ALWAYS=1" ]; then
    pass "GOO_NEVER->0 and GOO_ALWAYS->1 as constants, ignoring the argument"
else
    bad "coverage build: exit $st, got '$out', expected 'NEVER=0 ALWAYS=1'"
fi

# The argument must still be TYPE-CHECKED in the coverage build, or a
# GOO_NEVER whose expression stopped compiling would hide there and only break
# a release months later.
cat > "$WORK/typed.c" <<'EOF'
#include "goo_assert.h"
struct S { int a; };
int main(void) { struct S s = {0}; return GOO_NEVER(s.nonexistent_field); }
EOF
if $CC_ $CSTD -I"$ROOT/include" -DGOO_COVERAGE -c "$WORK/typed.c" -o "$WORK/t.o" 2>/dev/null; then
    bad "coverage build ACCEPTED a GOO_NEVER with a bad expression — argument is not type-checked"
else
    pass "a malformed GOO_NEVER argument is still a compile error under coverage"
fi

echo
if [ "$rc" -eq 0 ]; then
    echo "goo-assert-probe: PASS (all three build modes behave)"
else
    echo "goo-assert-probe: FAIL"
fi
exit "$rc"
