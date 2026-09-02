#!/bin/bash
# goo-assert probe: the four build modes of include/goo_assert.h.
#
# WHY THIS EXISTS. GOO_ASSERT, GOO_NEVER and GOO_ALWAYS are force-included into
# every translation unit and expand to DIFFERENT code across builds. That is
# the whole point of them, and it is also the whole risk: a default build
# where GOO_ASSERT does not abort ships a compiler where the checks are dead,
# and a debug build where it silently does nothing is a macro that looks like
# a check and is not one.
#
# Neither failure is visible by reading the header, because the preprocessor
# picks the arm. So compile the same fixtures four ways and observe them.
#
# THE FOUR MODES, since the 2026-09-02 flip (Task 24b): assertions are ON by
# default, and GOO_ASSERT_OFF is the opt-out kill switch, not the other way
# around. The measured cost of leaving them on was zero (docs/misra/README.md,
# Task 24's serial re-measurement), and Holzmann's Power of Ten rules argue an
# assertion belongs in the code that flies. See include/goo_assert.h for the
# full reasoning.
#
#   default (no flag)     GOO_ASSERT(0) MUST abort       -> SIGABRT
#                          GOO_NEVER(1)/GOO_ALWAYS(0) MUST abort too -- the
#                          checking arm is the default for all three macros.
#   GOO_ASSERT_OFF         GOO_ASSERT(0) must NOT abort   -> exit 0
#                          GOO_NEVER(x)/GOO_ALWAYS(x) pass x through, so a
#                          defensive branch is still THERE as a last defence.
#                          This is the pre-2026-09-02 default, kept alive only
#                          as the kill switch.
#   debug (GOO_DEBUG)      GOO_ASSERT(0) MUST abort       -> SIGABRT
#                          GOO_NEVER(1) MUST abort, GOO_ALWAYS(0) MUST abort.
#                          Unchanged by the flip: GOO_DEBUG already forced the
#                          checking arm on.
#   coverage (GOO_COVERAGE)
#                          GOO_ASSERT is a no-op, GOO_NEVER -> constant 0,
#                          GOO_ALWAYS -> constant 1, regardless of the
#                          argument. Unchanged by the flip: this is what keeps
#                          an unreachable defensive branch, and a never-failing
#                          assert, out of the coverage denominator.
#
# The default and coverage rows are the ones that would otherwise rot in
# silence: nothing else in the tree would notice if they stopped holding.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

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

# The combined fixture. Used only for the two modes where NOTHING aborts
# (GOO_ASSERT_OFF and coverage), so the print order below is safe to read in
# full. The two modes where something DOES abort (default, debug) instead use
# the single-macro fixtures further down, so a failure names exactly which
# macro did not fire -- reading an abort out of a multi-macro program cannot
# tell you which expression triggered it, since argument evaluation order is
# unspecified.
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
    GOO_ASSERT(zero);          // false: no-op only under GOO_ASSERT_OFF or GOO_COVERAGE
    printf("survived\n");
    return 0;
}
EOF

cat > "$WORK/assert0.c" <<'EOF'
#include "goo_assert.h"
int main(void) { volatile int zero = 0; GOO_ASSERT(zero); return 0; }
EOF

cat > "$WORK/never.c" <<'EOF'
#include "goo_assert.h"
int main(void) { volatile int one = 1; return GOO_NEVER(one); }
EOF

cat > "$WORK/always.c" <<'EOF'
#include "goo_assert.h"
int main(void) { volatile int zero = 0; return GOO_ALWAYS(zero); }
EOF

# $1 = source file under $WORK, $2 = extra compiler flags. Prints the run's
# exit status; the run's stdout lands in $WORK/out for the caller to inspect.
compile_run() {
    local src="$1" flags="$2"
    $CC_ $CSTD -I"$ROOT/include" $flags "$src" -o "$WORK/bin" 2>"$WORK/cc.err" || {
        echo "  compile FAILED with flags '$flags' for $src:" >&2
        sed 's/^/    /' "$WORK/cc.err" >&2
        echo 99
        return
    }
    "$WORK/bin" > "$WORK/out" 2>/dev/null
    # Status taken inside the function: a bare invocation would let the shell
    # print its own "Aborted (core dumped)" job-control line, which looks like
    # probe output and is not.
    echo $?
}

echo "=== goo-assert-probe: four build modes of include/goo_assert.h ==="

# ---------------------------------------------------------------------------
echo "--- default (no flag; assertions on since 2026-09-02) ---"
st=$(compile_run "$WORK/assert0.c" "")
if [ "$st" = "134" ]; then
    pass "GOO_ASSERT(0) aborts by default (SIGABRT, rc 134)"
else
    bad "default build exited $st, expected 134 (SIGABRT) — GOO_ASSERT is not on by default"
fi
st=$(compile_run "$WORK/never.c" "")
if [ "$st" = "134" ]; then
    pass "GOO_NEVER(true) aborts by default"
else
    bad "GOO_NEVER(true) exited $st under the default build, expected 134"
fi
st=$(compile_run "$WORK/always.c" "")
if [ "$st" = "134" ]; then
    pass "GOO_ALWAYS(false) aborts by default"
else
    bad "GOO_ALWAYS(false) exited $st under the default build, expected 134"
fi

# ---------------------------------------------------------------------------
echo "--- GOO_ASSERT_OFF (the kill switch) ---"
st=$(compile_run "$WORK/probe.c" "-DGOO_ASSERT_OFF")
out="$(cat "$WORK/out" 2>/dev/null)"
if [ "$st" = "0" ] && grep -q "survived" "$WORK/out" 2>/dev/null; then
    pass "GOO_ASSERT(0) does not abort under GOO_ASSERT_OFF (exit 0, reached 'survived')"
else
    bad "GOO_ASSERT_OFF build aborted or exited $st — the kill switch did not turn GOO_ASSERT off"
fi
# Pass-through is the property that keeps the defensive branch in a build
# made with the kill switch on. NEVER(1) must be 1 here, NOT the coverage
# build's constant 0.
if [ "$out" != "${out#NEVER=1 ALWAYS=0}" ]; then
    pass "GOO_NEVER/GOO_ALWAYS pass the condition through under GOO_ASSERT_OFF (NEVER=1 ALWAYS=0)"
else
    bad "expected 'NEVER=1 ALWAYS=0' under GOO_ASSERT_OFF, got: $(head -1 "$WORK/out" 2>/dev/null)"
fi

# ---------------------------------------------------------------------------
echo "--- debug (-DGOO_DEBUG) ---"
# 134 = 128 + SIGABRT(6). This is the row that proves an assert is a check and
# not a decoration. Unchanged by the 2026-09-02 flip: GOO_DEBUG already forced
# the checking arm on before it, and still does.
st=$(compile_run "$WORK/assert0.c" "-DGOO_DEBUG")
if [ "$st" = "134" ]; then
    pass "GOO_ASSERT(0) aborts (SIGABRT, rc 134)"
else
    bad "debug build exited $st, expected 134 (SIGABRT) — the assert did not fire"
fi
st=$(compile_run "$WORK/never.c" "-DGOO_DEBUG")
[ "$st" = "134" ] && pass "GOO_NEVER(true) aborts" || bad "GOO_NEVER(true) exited $st, expected 134"
st=$(compile_run "$WORK/always.c" "-DGOO_DEBUG")
[ "$st" = "134" ] && pass "GOO_ALWAYS(false) aborts" || bad "GOO_ALWAYS(false) exited $st, expected 134"

# ---------------------------------------------------------------------------
echo "--- coverage (-DGOO_COVERAGE) ---"
st=$(compile_run "$WORK/probe.c" "-DGOO_COVERAGE")
out="$(head -1 "$WORK/out" 2>/dev/null)"
if [ "$st" = "0" ] && [ "$out" = "NEVER=0 ALWAYS=1" ]; then
    pass "GOO_NEVER->0 and GOO_ALWAYS->1 as constants, ignoring the argument (GOO_ASSERT stayed a no-op too)"
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
    echo "goo-assert-probe: PASS (all four build modes behave)"
else
    echo "goo-assert-probe: FAIL"
fi
exit "$rc"
