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
#   4. Each invariant check of tests/sim/sim_os.c fires on its own violation
#      (spec section 7). Read from the exit status AND the trace header's
#      verdict and detail, so the check that fired is the one the case broke.
#      Seeds 1..4 each, because a case with two threads must fire on every
#      interleaving, not on one lucky seed.
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
for seed in 1 2 3 4 5 6 7 8 33 58; do
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
    # The first "start" record is the real thread (-) picking coroutine 0.
    # Seeded preemption (spec section 6) can deschedule coroutine 0 at that
    # decision first, so "preempt 0" records by the real thread may come
    # before it, and nothing else may. Step and time are not compared: with
    # preemption on, each decision costs virtual time. Seeds 33 and 58 have
    # a preempt record first at the default ppm.
    first_start="$(awk 'NR > 2 && $4 == "start" { print $3 " " $5; exit }
        NR > 2 && !($3 == "-" && $4 == "preempt" && $5 == "0") { print "other: " $0; exit }' "$trace")"
    [ "$first_start" = "- 0" ] || fail "coroutine0 seed=$seed: want the first start record '- start 0' after only 'preempt 0' records; got '$first_start'"
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

# Fact 4. $1 case, $2 exit status, $3 the header's "verdict=... detail=..."
# tail, which ends the header line.
expect_verdict() {
    local seed header
    for seed in 1 2 3 4; do
        run_case "$1" "$seed"
        header="$(head -1 "$WORK/trace/$seed.trace" 2>/dev/null)"
        [ "$RC" -eq "$2" ] || fail "$1 seed=$seed exit=$RC (want $2); stderr: $(head -c 300 "$WORK/$1.$seed.err")"
        case "$header" in
            *" $3") ;;
            *) fail "$1 seed=$seed: trace header does not end with '$3': $header" ;;
        esac
    done
}
expect_verdict lock-order             11 "verdict=invariant detail=lock-order"
expect_verdict recursive-lock         11 "verdict=invariant detail=recursive-lock"
expect_verdict unlock-not-held        11 "verdict=invariant detail=unlock-not-held"
expect_verdict wait-without-mutex     11 "verdict=invariant detail=wait-without-mutex"
expect_verdict wait-other-owner       11 "verdict=invariant detail=wait-without-mutex"
expect_verdict exit-holding-mutex     11 "verdict=invariant detail=exit-holding-mutex"
expect_verdict free-held-mutex        11 "verdict=invariant detail=free-held-mutex"
expect_verdict free-cond-with-waiters 11 "verdict=invariant detail=free-cond-with-waiters"
expect_verdict join-self              11 "verdict=invariant detail=join-self"
expect_verdict join-detached          11 "verdict=invariant detail=join-detached"
expect_verdict unknown-mutex          11 "verdict=invariant detail=unknown-mutex"
expect_verdict limit-mutexes          15 "verdict=limit-mutexes detail=mutexes"

if [ "$FAILS" -ne 0 ]; then
    echo "sim-entry-test: FAIL ($FAILS checks, $BIN, work dir $WORK)"
    exit 1
fi
echo "sim-entry-test: PASS ($BIN: main is coroutine 0 on seeds 1-8, 33 and 58, the exit path stays on coroutine 0, status 3 passes through, stdout survives sim-deadlock, 12 violation cases fire their own check on seeds 1-4)"
