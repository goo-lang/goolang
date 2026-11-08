#!/usr/bin/env bash
# deadlock-poll-probe — a real deadlock aborts THROUGH the poll loop.
#
# WHAT NO OTHER GATE REACHED. The 2026-09-02 review of task 8 (the detector
# rewrite) found that the tree had no gate on goo_scheduler_wait's poll loop:
#
#   - deadlock-probe runs examples/deadlock_probe.goo, where MAIN blocks on an
#     empty channel with no goroutine alive. That aborts inside
#     goo_sched_block_begin's main-thread branch, before any poll happens. It
#     spawns no goroutine, so blocked_goroutines is never compared with
#     num_goroutines and GOO_DEADLOCK_POLL_STREAK is never read.
#   - false-deadlock-probe asserts the ABSENCE of an abort. It proves the count
#     can stay below num_goroutines; it cannot prove the abort still fires.
#
# So the threshold task 8 moved from 100 to 3, and the `all_asleep`
# conjunction it guards, had no C-level positive control. This probe is it.
#
# WHAT IS ASSERTED, on tests/concurrency/deadlock_poll_test.c: two goroutines
# park for ever on an unbuffered channel with no sender, main calls
# goo_scheduler_wait, and the process must leave through goo_deadlock_abort:
# exit 2, Go's message on stderr, and nothing on stdout (a woken goroutine
# prints there, so silence is the proof that neither parker was released).
#
# TEETH. --self-test links a MUTATED runtime translation unit ahead of the
# archive, which the linker prefers, so one function changes and nothing else
# does. Two mutants, each aimed at one assertion:
#
#   1. GOO_DEADLOCK_POLL_STREAK raised past the timeout. The same program then
#      HANGS. This is also the evidence that the abort comes from the poll loop
#      and not from another site: no other abort site depends on that constant.
#   2. goo_deadlock_abort's message text changed. The program still exits 2, so
#      only the message assertion can see it.
#
# A control build, compiled from the TRACKED sources and linked the same way,
# must stay green. Without it a broken harness would turn both mutants red and
# read as success.
set -u

PROBE="deadlock-poll-probe"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

CC_="${CC_PROBE:-gcc}"
CSTD="${CSTD_PROBE:--std=c23}"
RUNTIME_LIB="${RUNTIME_LIB_PROBE:-lib/libgoo_runtime.a}"
TEST_SRC="tests/concurrency/deadlock_poll_test.c"
SCHED_SRC="src/runtime/concurrency.c"
ABORT_SRC="src/runtime/deadlock.c"
# 10 s is 20,000 polls of the detector's 0.5 ms loop against a threshold of 3.
# A hang here is a verdict, not an accident of a slow machine.
TIMEOUT="${DEADLOCK_POLL_TIMEOUT:-10}"
MSG="all goroutines are asleep - deadlock!"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# The flags the Makefile compiles a runtime object with. src/runtime holds
# platform.h, and the two -include headers carry GOO_ASSERT and the xalloc
# wrappers, so a translation unit compiled without them is a different program.
RT_CFLAGS="-D_GNU_SOURCE -I. -Iinclude -Isrc/runtime -include include/xalloc.h -include include/goo_assert.h"

# Compiles one runtime source into an object. The mutants go through here too.
compile_rt() {  # $1 = source, $2 = out
    # shellcheck disable=SC2086
    $CC_ $CSTD $RT_CFLAGS -c "$1" -o "$2" 2>"$WORK/cc.err"
}

# Links the test. $1 may name an object to place BEFORE the archive: the
# linker resolves every symbol it defines from it and never pulls the
# archive's copy, so one translation unit is replaced and the rest of the
# runtime is the shipped one.
build() {  # $1 = extra object or "", $2 = out
    # shellcheck disable=SC2086
    $CC_ $CSTD -D_GNU_SOURCE -Iinclude -I. "$TEST_SRC" $1 "$RUNTIME_LIB" \
        -lpthread -lm -o "$2" 2>"$WORK/cc.err"
}

# Runs one binary and applies the four assertions. Prints one indented line per
# assertion that failed. Returns 0 green, 1 red.
judge() {  # $1 = binary, $2 = label
    local bin="$1" label="$2" rc
    timeout "$TIMEOUT" "$bin" >"$WORK/run.out" 2>"$WORK/run.err"
    rc=$?
    if [ "$rc" -eq 124 ]; then
        echo "    $label: HANG — goo_scheduler_wait polled for ${TIMEOUT}s and never aborted"
        return 1
    fi
    if [ "$rc" -ne 2 ]; then
        echo "    $label: exit $rc, expected 2"
        sed 's/^/      /' "$WORK/run.err"
        return 1
    fi
    if [ -s "$WORK/run.out" ]; then
        echo "    $label: a parked goroutine woke (stdout is not empty)"
        sed 's/^/      /' "$WORK/run.out"
        return 1
    fi
    if ! grep -q "$MSG" "$WORK/run.err"; then
        echo "    $label: exit 2 without the message \"$MSG\""
        sed 's/^/      /' "$WORK/run.err"
        return 1
    fi
    return 0
}

echo "=== $PROBE: two parked goroutines abort through goo_scheduler_wait (exit 2) ==="

if [ ! -r "$RUNTIME_LIB" ]; then
    echo "$PROBE: TOOL FAILURE cannot read $RUNTIME_LIB (run make runtime-lib)"
    exit 2
fi

# ---------------------------------------------------------------------------
# Teeth.
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--self-test" ]; then
    SELF="$PROBE --self-test"
    before_sched="$(sha256sum "$SCHED_SRC" | cut -d' ' -f1)"
    before_abort="$(sha256sum "$ABORT_SRC" | cut -d' ' -f1)"

    # Mutant 1: the threshold, far past the timeout. sed on the one declaration.
    sed 's/const int GOO_DEADLOCK_POLL_STREAK = [0-9]*;/const int GOO_DEADLOCK_POLL_STREAK = 100000000;/' \
        "$SCHED_SRC" > "$WORK/sched_mutant.c"
    if [ "$(grep -c 'GOO_DEADLOCK_POLL_STREAK = 100000000;' "$WORK/sched_mutant.c")" -ne 1 ]; then
        echo "  SELF-TEST BROKEN — the poll threshold declaration in $SCHED_SRC"
        echo "  no longer matches, so mutant 1 changes nothing and proves nothing."
        exit 1
    fi

    # Mutant 2: the abort message. The exit status stays 2.
    sed "s/$MSG/all goroutines are having a nap/" "$ABORT_SRC" > "$WORK/abort_mutant.c"
    if [ "$(grep -c 'all goroutines are having a nap' "$WORK/abort_mutant.c")" -ne 1 ]; then
        echo "  SELF-TEST BROKEN — the abort message in $ABORT_SRC no longer matches."
        exit 1
    fi

    [ "$before_sched" = "$(sha256sum "$SCHED_SRC" | cut -d' ' -f1)" ] || {
        echo "  SELF-TEST BROKEN — tracked $SCHED_SRC was modified"; exit 1; }
    [ "$before_abort" = "$(sha256sum "$ABORT_SRC" | cut -d' ' -f1)" ] || {
        echo "  SELF-TEST BROKEN — tracked $ABORT_SRC was modified"; exit 1; }

    rc=0

    echo "--- Control: tracked $SCHED_SRC, compiled and linked the mutant way ---"
    if compile_rt "$SCHED_SRC" "$WORK/ctl.o" && build "$WORK/ctl.o" "$WORK/ctl_bin"; then
        if judge "$WORK/ctl_bin" "control"; then
            echo "    ok: the control aborts (exit 2, message present)"
        else
            echo "  CONTROL FAILED — the harness is broken, not the tree. Both"
            echo "  mutants below would read red for the wrong reason."
            rc=1
        fi
    else
        echo "  CONTROL FAILED — the control did not build"
        sed 's/^/    /' "$WORK/cc.err"
        rc=1
    fi

    echo "--- Mutant 1: GOO_DEADLOCK_POLL_STREAK = 100000000 -> MUST hang ---"
    if compile_rt "$WORK/sched_mutant.c" "$WORK/m1.o" && build "$WORK/m1.o" "$WORK/m1_bin"; then
        if judge "$WORK/m1_bin" "mutant 1"; then
            echo "  MUTANT 1 STAYED GREEN — the probe passes with the poll"
            echo "  threshold unreachable, so its exit 2 does not come from the"
            echo "  poll loop and this gate proves nothing about it."
            rc=1
        else
            echo "    ok: raising the threshold turns the probe red"
        fi
    else
        echo "  MUTANT 1 FAILED TO BUILD"
        sed 's/^/    /' "$WORK/cc.err"
        rc=1
    fi

    echo "--- Mutant 2: goo_deadlock_abort's message changed -> MUST fail ---"
    if compile_rt "$WORK/abort_mutant.c" "$WORK/m2.o" && build "$WORK/m2.o" "$WORK/m2_bin"; then
        if judge "$WORK/m2_bin" "mutant 2"; then
            echo "  MUTANT 2 STAYED GREEN — the message assertion does not read"
            echo "  the message."
            rc=1
        else
            echo "    ok: changing the message turns the probe red"
        fi
    else
        echo "  MUTANT 2 FAILED TO BUILD"
        sed 's/^/    /' "$WORK/cc.err"
        rc=1
    fi

    echo
    if [ "$rc" -eq 0 ]; then
        echo "$SELF: PASS (control green on $TEST_SRC; threshold and message mutants each turn it red)"
    else
        echo "$SELF: FAIL (see above)"
    fi
    exit "$rc"
fi

# ---------------------------------------------------------------------------
# Ordinary run: the shipped runtime archive, no substituted object.
# ---------------------------------------------------------------------------
mkdir -p build
BIN="build/deadlock_poll_test"
if ! build "" "$BIN"; then
    echo "$PROBE: FAIL ($TEST_SRC did not build against $RUNTIME_LIB)"
    sed 's/^/    /' "$WORK/cc.err"
    exit 1
fi

if judge "$BIN" "$PROBE"; then
    echo "$PROBE: PASS ($BIN against $RUNTIME_LIB, 2 goroutines parked on an unbuffered channel, exit 2, \"$MSG\", stdout empty)"
    exit 0
fi
echo "$PROBE: FAIL ($BIN against $RUNTIME_LIB — see the line above)"
exit 1
