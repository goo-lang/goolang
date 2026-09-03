#!/usr/bin/env bash
# false-deadlock-probe — the detector must NOT abort a LIVE program.
#
# The negative control for deadlock-probe and deadlock-poll-probe, and the
# C-level counterpart of the SPIN model in tests/models/ (see
# tests/models/README.md, "The C-level fix").
#
# WHAT IS ASSERTED, on tests/concurrency/false_deadlock_test.c: one sender and
# one receiver hand four values over an unbuffered channel while
# GOO_TEST_WAKE_DELAY_NS holds the wake-to-unregister window open for 100 ms,
# which is 200 polls of the detector's 0.5 ms loop. The program must exit 0 and
# print its OK line. Before 2026-09-02 it exited 2 with "all goroutines are
# asleep - deadlock!" while both goroutines were alive.
#
# WHY THIS IS A SCRIPT AND NOT A MAKEFILE RECIPE. It was a recipe until
# 2026-09-03. probe-teeth-probe scans scripts/*.sh, so a recipe is outside the
# scan by construction: this gate had no in-tree negative control, and nothing
# said so. The 2026-09-02 review of task 8 raised it. A probe that has only
# ever been green has not shown it can report red.
#
# TEETH. --self-test links a MUTATED runtime translation unit ahead of the
# archive, which the linker prefers, so exactly one function changes:
#
#   1. goo_sched_wake_waiters returns without unregistering anybody. That is
#      the pre-2026-09-02 accounting restored — a woken goroutine stays in the
#      blocked set until it is next scheduled — and it must bring back the
#      false abort this gate exists to forbid.
#   2. The run with GOO_TEST_WAKE_DELAY_NS unset. The window is then
#      microseconds wide and a green result would prove nothing, so the test
#      refuses to run and the probe must report that refusal as a failure.
#
# A control build from the TRACKED sources, linked the same way, must stay
# green: without it a broken harness turns the mutants red and reads as
# success.
set -u

PROBE="false-deadlock-probe"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

CC_="${CC_PROBE:-gcc}"
CSTD="${CSTD_PROBE:--std=c23}"
RUNTIME_LIB="${RUNTIME_LIB_PROBE:-lib/libgoo_runtime.a}"
TEST_SRC="tests/concurrency/false_deadlock_test.c"
SCHED_SRC="src/runtime/concurrency.c"
# 100 ms per wake, 4 handovers, 2 wakes each: the green run is under a second.
#
# The override is NOT the runtime's own GOO_TEST_WAKE_DELAY_NS. Reading that
# one would let an ambient value in the operator's environment narrow the
# window to microseconds and turn this gate into a green that proves nothing,
# silently. The floor below refuses a window narrower than 20 polls for the
# same reason.
DELAY_NS="${FALSE_DEADLOCK_DELAY_NS:-100000000}"
DELAY_FLOOR_NS=10000000   # 10 ms = 20 polls of the detector's 0.5 ms loop
TIMEOUT="${FALSE_DEADLOCK_TIMEOUT:-60}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# The flags the Makefile compiles a runtime object with. src/runtime holds
# platform.h, and the two -include headers carry GOO_ASSERT and the xalloc
# wrappers, so a translation unit compiled without them is a different program.
RT_CFLAGS="-D_GNU_SOURCE -I. -Iinclude -Isrc/runtime -include include/xalloc.h -include include/goo_assert.h"

compile_rt() {  # $1 = source, $2 = out
    # shellcheck disable=SC2086
    $CC_ $CSTD $RT_CFLAGS -c "$1" -o "$2" 2>"$WORK/cc.err"
}

# $1 may name an object placed BEFORE the archive: the linker resolves every
# symbol it defines from it and never pulls the archive's copy.
build() {  # $1 = extra object or "", $2 = out
    # shellcheck disable=SC2086
    $CC_ $CSTD -D_GNU_SOURCE -Iinclude -I. "$TEST_SRC" $1 "$RUNTIME_LIB" \
        -lpthread -lm -o "$2" 2>"$WORK/cc.err"
}

# Runs one binary and applies both assertions. Returns 0 green, 1 red.
judge() {  # $1 = binary, $2 = label, $3 = delay ("" leaves the hook unset)
    local bin="$1" label="$2" delay="$3" rc
    if [ -n "$delay" ]; then
        GOO_TEST_WAKE_DELAY_NS="$delay" timeout "$TIMEOUT" "$bin" \
            >"$WORK/run.out" 2>"$WORK/run.err"
    else
        env -u GOO_TEST_WAKE_DELAY_NS timeout "$TIMEOUT" "$bin" \
            >"$WORK/run.out" 2>"$WORK/run.err"
    fi
    rc=$?
    if [ "$rc" -eq 124 ]; then
        echo "    $label: HANG — the program did not finish in ${TIMEOUT}s"
        return 1
    fi
    if [ "$rc" -ne 0 ]; then
        echo "    $label: exit $rc, expected 0"
        sed 's/^/      /' "$WORK/run.err"
        return 1
    fi
    if ! grep -q "^false-deadlock-probe OK" "$WORK/run.out"; then
        echo "    $label: exit 0 without the OK line"
        sed 's/^/      /' "$WORK/run.out"
        return 1
    fi
    return 0
}

echo "=== $PROBE: a live program survives a ${DELAY_NS}ns wake delay ==="

if [ ! -r "$RUNTIME_LIB" ]; then
    echo "$PROBE: TOOL FAILURE cannot read $RUNTIME_LIB (run make runtime-lib)"
    exit 2
fi

# A window narrower than the floor makes a green run meaningless, so it is a
# tool failure and not a pass.
case "$DELAY_NS" in
    ''|*[!0-9]*)
        echo "$PROBE: TOOL FAILURE FALSE_DEADLOCK_DELAY_NS=\"$DELAY_NS\" is not a nanosecond count"
        exit 2 ;;
esac
if [ "$DELAY_NS" -lt "$DELAY_FLOOR_NS" ]; then
    echo "$PROBE: TOOL FAILURE wake delay ${DELAY_NS}ns is below the ${DELAY_FLOOR_NS}ns floor"
    exit 2
fi

# ---------------------------------------------------------------------------
# Teeth.
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--self-test" ]; then
    SELF="$PROBE --self-test"
    before_sched="$(sha256sum "$SCHED_SRC" | cut -d' ' -f1)"

    # Mutant 1: the wake site stops unregistering. Everything else, including
    # goo_sched_block_end's own unregister, is untouched — so the count is
    # released when the woken goroutine is next scheduled, which is exactly the
    # protocol this gate was written against.
    sed 's|^void goo_sched_wake_waiters(goo_goroutine_t\*\* waiters) {|void goo_sched_wake_waiters(goo_goroutine_t** waiters) { (void)waiters; return;|' \
        "$SCHED_SRC" > "$WORK/sched_mutant.c"
    if [ "$(grep -c '(void)waiters; return;' "$WORK/sched_mutant.c")" -ne 1 ]; then
        echo "  SELF-TEST BROKEN — goo_sched_wake_waiters' signature in"
        echo "  $SCHED_SRC no longer matches, so mutant 1 changes nothing."
        exit 1
    fi

    [ "$before_sched" = "$(sha256sum "$SCHED_SRC" | cut -d' ' -f1)" ] || {
        echo "  SELF-TEST BROKEN — tracked $SCHED_SRC was modified"; exit 1; }

    rc=0

    echo "--- Control: tracked $SCHED_SRC, compiled and linked the mutant way ---"
    if compile_rt "$SCHED_SRC" "$WORK/ctl.o" && build "$WORK/ctl.o" "$WORK/ctl_bin"; then
        if judge "$WORK/ctl_bin" "control" "$DELAY_NS"; then
            echo "    ok: the control survives the wake delay (exit 0, OK line)"
        else
            echo "  CONTROL FAILED — the harness is broken, not the tree."
            rc=1
        fi
    else
        echo "  CONTROL FAILED — the control did not build"
        sed 's/^/    /' "$WORK/cc.err"
        rc=1
    fi

    echo "--- Mutant 1: goo_sched_wake_waiters unregisters nobody -> MUST abort ---"
    if compile_rt "$WORK/sched_mutant.c" "$WORK/m1.o" && build "$WORK/m1.o" "$WORK/m1_bin"; then
        if judge "$WORK/m1_bin" "mutant 1" "$DELAY_NS"; then
            echo "  MUTANT 1 STAYED GREEN — the old accounting no longer aborts"
            echo "  this program, so a green run says nothing about the fix."
            rc=1
        else
            echo "    ok: the pre-fix accounting turns the probe red"
        fi
    else
        echo "  MUTANT 1 FAILED TO BUILD"
        sed 's/^/    /' "$WORK/cc.err"
        rc=1
    fi

    echo "--- Mutant 2: the wake delay unset -> MUST fail, never pass blind ---"
    if build "" "$WORK/m2_bin"; then
        if judge "$WORK/m2_bin" "mutant 2" ""; then
            echo "  MUTANT 2 STAYED GREEN — the probe passes with the window"
            echo "  microseconds wide, which is a green that proves nothing."
            rc=1
        else
            echo "    ok: a run with no wake delay turns the probe red"
        fi
    else
        echo "  MUTANT 2 FAILED TO BUILD"
        sed 's/^/    /' "$WORK/cc.err"
        rc=1
    fi

    echo
    if [ "$rc" -eq 0 ]; then
        echo "$SELF: PASS (control green on $TEST_SRC; the pre-fix wake site and the unset hook each turn it red)"
    else
        echo "$SELF: FAIL (see above)"
    fi
    exit "$rc"
fi

# ---------------------------------------------------------------------------
# Ordinary run: the shipped runtime archive, no substituted object.
# ---------------------------------------------------------------------------
mkdir -p build
BIN="build/false_deadlock_test"
if ! build "" "$BIN"; then
    echo "$PROBE: FAIL ($TEST_SRC did not build against $RUNTIME_LIB)"
    sed 's/^/    /' "$WORK/cc.err"
    exit 1
fi

if judge "$BIN" "$PROBE" "$DELAY_NS"; then
    echo "$PROBE: PASS ($BIN against $RUNTIME_LIB, 4 rendezvous handovers, ${DELAY_NS}ns wake delay, exit 0, OK line present)"
    exit 0
fi
echo "$PROBE: FAIL ($BIN against $RUNTIME_LIB — see the line above)"
exit 1
