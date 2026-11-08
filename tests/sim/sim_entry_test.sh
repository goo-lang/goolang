#!/usr/bin/env bash
# tests/sim/sim_entry_test.sh — judges the simulator's process entry
# (tests/sim/sim_main.c) through tests/sim/sim_entry_test.c, a program
# built like every program under test.
#
# Three facts, each read from an artefact the run leaves behind:
#   1. The program's main runs as coroutine 0 (spec section 6). Read from the
#      program's own output and from the trace's first record.
#   2. No seam call runs on the real thread after the program's main returns:
#      the exit path makes seam calls in an atexit handler while the runtime's
#      workers are live, and each such call asserts that it runs on a
#      coroutine. Read from the exit status (0, not SIGABRT) and the handler's
#      line. Seeds 1..8, because the handler's sleep lets the workers run and
#      the seed picks how.
#   3. The program's stdio output survives a non-pass verdict, which ends the
#      process with _exit.
# The instrument of fact 2 -- sim_current() aborts on the real thread -- is
# proved able to fire by T4 in tests/sim/sim_sched_test.c.
#
# Usage: tests/sim/sim_entry_test.sh <program> <work-dir>
set -u

BIN="${1:?usage: sim_entry_test.sh <program> <work-dir>}"
WORK="${2:?usage: sim_entry_test.sh <program> <work-dir>}"
FAILS=0

rm -rf "$WORK"
mkdir -p "$WORK/trace" || exit 2

fail() { echo "sim-entry-test: FAIL: $*"; FAILS=$((FAILS + 1)); }

# $1 case, $2 seed. Sets RC; the output is in $WORK/<case>.<seed>.out.
run_case() {
    SIM_ENTRY_CASE="$1" GOO_SIM_SEED="$2" GOO_SIM_TRACE_DIR="$WORK/trace" \
        "$BIN" > "$WORK/$1.$2.out" 2> "$WORK/$1.$2.err"
    RC=$?
}

# $1 file, $2 fixed string: 0 when the file holds the string as a whole line.
has_line() { grep -qxF -- "$2" "$1"; }

# Fact 1 and fact 2.
for seed in 1 2 3 4 5 6 7 8; do
    run_case coroutine0 "$seed"
    out="$WORK/coroutine0.$seed.out"
    trace="$WORK/trace/$seed.trace"
    [ "$RC" -eq 0 ] || fail "coroutine0 seed=$seed exit=$RC (want 0); stderr: $(head -c 300 "$WORK/coroutine0.$seed.err")"
    has_line "$out" "main-coroutine=0" || fail "coroutine0 seed=$seed: no 'main-coroutine=0' line"
    has_line "$out" "atexit-coroutine=0" || fail "coroutine0 seed=$seed: no 'atexit-coroutine=0' line"
    if [ ! -f "$trace" ]; then
        fail "coroutine0 seed=$seed: no trace at $trace"
        continue
    fi
    head -1 "$trace" | grep -q ' verdict=pass ' || fail "coroutine0 seed=$seed: trace header is not verdict=pass: $(head -1 "$trace")"
    # Line 3 is the first record: step 1, time 0, the real thread (-) at
    # "start", and the pick is coroutine 0.
    [ "$(sed -n 3p "$trace")" = "1 0 - start 0" ] || fail "coroutine0 seed=$seed: first record is '$(sed -n 3p "$trace")', want '1 0 - start 0'"
    # The handler's sleep is a seam call made after main returned: the trace
    # must show coroutine 0 at a sleep, so the exit path did reach the seam.
    grep -q '^[0-9]* [0-9]* 0 sleep ' "$trace" || fail "coroutine0 seed=$seed: the trace shows no sleep by coroutine 0"
done

# The exit status passes through unchanged.
run_case nonzero 1
[ "$RC" -eq 3 ] || fail "nonzero exit=$RC (want 3)"
has_line "$WORK/nonzero.1.out" "nonzero-returns=3" || fail "nonzero: no 'nonzero-returns=3' line"
head -1 "$WORK/trace/1.trace" | grep -q ' verdict=program-exit ' || fail "nonzero: trace header is not verdict=program-exit: $(head -1 "$WORK/trace/1.trace")"

# Fact 3.
run_case verdict-output 1
[ "$RC" -eq 10 ] || fail "verdict-output exit=$RC (want 10, sim-deadlock)"
has_line "$WORK/verdict-output.1.out" "output-before-verdict" || fail "verdict-output: the program's stdout line was lost at the verdict"

if [ "$FAILS" -ne 0 ]; then
    echo "sim-entry-test: FAIL ($FAILS checks, $BIN, work dir $WORK)"
    exit 1
fi
echo "sim-entry-test: PASS ($BIN: main is coroutine 0 on seeds 1-8, the exit path stays on coroutine 0, status 3 passes through, stdout survives sim-deadlock)"
