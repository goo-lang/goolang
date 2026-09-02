#!/usr/bin/env bash
# run_deadlock_model.sh — build and run the SPIN model of the Goo runtime's
# deadlock detector (tests/models/deadlock_detector.pml).
#
# Usage: run_deadlock_model.sh N K [DETECTOR]
#   N         goroutines, 2..4
#   K         asleep_streak threshold, >= 1
#   DETECTOR  1 (default) detector on, 0 detector never aborts (positive control)
#
# Environment:
#   PAN_ARGS  extra flags passed to ./pan (for example "-m100000 -w26")
#   CC        C compiler, flags allowed (default cc; e.g. CC='cc -DMEMLIM=1')
#   KEEP_DIR  directory to copy the model, pan sources and any .trail into
#             (the work directory itself is always a fresh mktemp -d and is
#             removed on exit, so pan.* files never land in the tree)
#
# Exit status:
#   0  pan completed the search and reported 0 errors
#   1  pan reported at least one error (assertion violation or invalid end state)
#   2  bad arguments, spin / cc / pan failed to run, or the run was
#      INCONCLUSIVE: pan found no error but did not finish the search, or it
#      explored 0 states. Such a run proves nothing, so it is never a pass.

set -u

usage() {
    echo "usage: $0 N K [DETECTOR]" >&2
    echo "  N in 2..4, K >= 1, DETECTOR in 0..1" >&2
}

die_usage() {
    echo "run_deadlock_model: $1" >&2
    usage
    exit 2
}

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    die_usage "expected 2 or 3 arguments, got $#"
fi

N=$1
K=$2
DETECTOR=${3:-1}

case "$N" in
    2|3|4) ;;
    *) die_usage "N must be 2, 3 or 4 (got '$N')" ;;
esac
case "$K" in
    ''|*[!0-9]*) die_usage "K must be a positive integer (got '$K')" ;;
esac
if [ "$K" -lt 1 ]; then
    die_usage "K must be >= 1 (got '$K')"
fi
case "$DETECTOR" in
    0|1) ;;
    *) die_usage "DETECTOR must be 0 or 1 (got '$DETECTOR')" ;;
esac

HERE=$(cd -- "$(dirname -- "$0")" && pwd)
REPO=$(cd -- "$HERE/../.." && pwd)
MODEL="$HERE/deadlock_detector.pml"

if [ ! -f "$MODEL" ]; then
    echo "run_deadlock_model: model not found: $MODEL" >&2
    exit 2
fi

SPIN=$(bash "$REPO/scripts/spin_bootstrap.sh" 2>/dev/null | tail -1)
if [ -z "${SPIN:-}" ] || [ ! -x "$SPIN" ]; then
    echo "run_deadlock_model: spin_bootstrap.sh gave no usable spin binary" >&2
    exit 2
fi
SPIN_VERSION=$("$SPIN" -V 2>&1 | head -1)

CC=${CC:-cc}
PAN_ARGS=${PAN_ARGS:-}

WORK=$(mktemp -d)
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

cp "$MODEL" "$WORK/deadlock_detector.pml"

DEFS="-DN=$N -DK=$K -DDETECTOR=$DETECTOR"

if ! ( cd "$WORK" && "$SPIN" $DEFS -a deadlock_detector.pml ) > "$WORK/spin.log" 2>&1; then
    echo "run_deadlock_model: spin -a failed" >&2
    cat "$WORK/spin.log" >&2
    exit 2
fi
if [ ! -f "$WORK/pan.c" ]; then
    echo "run_deadlock_model: spin -a produced no pan.c" >&2
    cat "$WORK/spin.log" >&2
    exit 2
fi

# $CC is deliberately unquoted, the way make treats CC: it may carry flags, so
# CC='cc -DMEMLIM=1' works. The verdict line prints the value, not just the
# compiler's own --version, or a wrapper's flags would be invisible.
if ! ( cd "$WORK" && $CC -O2 -DSAFETY -o pan pan.c ) > "$WORK/cc.log" 2>&1; then
    echo "run_deadlock_model: cc failed" >&2
    cat "$WORK/cc.log" >&2
    exit 2
fi

START=$(date +%s.%N)
( cd "$WORK" && ./pan $PAN_ARGS ) > "$WORK/pan.log" 2>&1
PAN_RC=$?
END=$(date +%s.%N)
WALL=$(awk -v a="$START" -v b="$END" 'BEGIN { printf "%.2f", b - a }')

ERRORS=$(sed -n 's/.*errors: \([0-9][0-9]*\).*/\1/p' "$WORK/pan.log" | tail -1)
STATES=$(sed -n 's/^ *\([0-9][0-9]*\) states, stored.*/\1/p' "$WORK/pan.log" | tail -1)
DEPTH=$(sed -n 's/.*depth reached \([0-9][0-9]*\).*/\1/p' "$WORK/pan.log" | tail -1)

if [ -z "${ERRORS:-}" ] || [ -z "${STATES:-}" ]; then
    echo "run_deadlock_model: pan produced no verdict (rc=$PAN_RC)" >&2
    cat "$WORK/pan.log" >&2
    exit 2
fi

if [ -n "${KEEP_DIR:-}" ]; then
    mkdir -p "$KEEP_DIR"
    cp "$WORK/deadlock_detector.pml" "$WORK/pan.log" "$KEEP_DIR/" 2>/dev/null
    cp "$WORK"/*.trail "$KEEP_DIR/" 2>/dev/null
    if [ -f "$WORK/deadlock_detector.pml.trail" ]; then
        ( cd "$WORK" && "$SPIN" -t -p $DEFS deadlock_detector.pml ) \
            > "$KEEP_DIR/deadlock_detector.trace.txt" 2>&1
    fi
fi

# pan prints "invalid end states  +" in its option header, so match only the
# report form "pan:<n>: invalid end state (at depth ...)".
INVALID_END=$(grep -c '^pan:[0-9]*: invalid end state' "$WORK/pan.log")

# Did pan finish the search? pan says so in more than one way, and it does NOT
# use the same words for each cause:
#   "Warning: Search not completed"       (stopped at the first error, and the
#                                          -DMEMLIM stop)
#   "error: max search depth too small"   (the -m depth limit; this one prints
#                                          NO "Search not completed" line)
#   "pan: reached -DMEMLIM bound"         (the memory limit; can stop after 0
#                                          states explored)
# Enumerating causes is a losing game, so the last pattern is the catch-all
# "^error:", which covers pan's whole family of early-stop messages. The
# 2026-09-02 review found the memory case by building pan with -DMEMLIM=1: the
# runner printed PASS with states=0 and exited 0.
COMPLETED=yes
if grep -qE '^(Warning: )?Search not completed|^error:|^pan: reached' "$WORK/pan.log"; then
    COMPLETED=no
fi

# ONE rule, and it does not care WHY the search stopped early: a search that
# did not finish and found nothing proves nothing, so it is never a PASS. The
# cause goes into the message only.
INCONCLUSIVE_WHY=
if [ "$ERRORS" -eq 0 ] && [ "$COMPLETED" = no ]; then
    INCONCLUSIVE_WHY="pan did not complete the search and found no error"
elif [ "$ERRORS" -eq 0 ] && [ "$STATES" -eq 0 ]; then
    INCONCLUSIVE_WHY="pan explored 0 states"
fi

if [ -n "$INCONCLUSIVE_WHY" ]; then
    REASON=$(grep -m1 -E 'max search depth too small|reached -DMEMLIM bound|out of memory|error:' "$WORK/pan.log")
    echo "run_deadlock_model: INCONCLUSIVE model=tests/models/deadlock_detector.pml N=$N K=$K DETECTOR=$DETECTOR errors=$ERRORS states=$STATES search_completed=$COMPLETED -- $INCONCLUSIVE_WHY, so it proves nothing" >&2
    if [ -n "$REASON" ]; then
        echo "run_deadlock_model:   pan said: $REASON" >&2
    fi
    echo "run_deadlock_model:   raise the depth with PAN_ARGS='-m<depth>', raise the memory with PAN_ARGS='-w<n>', or drop a -DMEMLIM from CC" >&2
    exit 2
fi

if [ "$ERRORS" -eq 0 ]; then
    VERDICT=PASS
    RC=0
else
    VERDICT=FAIL
    RC=1
fi

echo "run_deadlock_model: $VERDICT model=tests/models/deadlock_detector.pml N=$N K=$K DETECTOR=$DETECTOR errors=$ERRORS invalid_end_states=$INVALID_END states=$STATES search_completed=$COMPLETED depth=$DEPTH wall=${WALL}s spin='$SPIN_VERSION' CC='$CC' cc='$($CC --version 2>/dev/null | head -1)'"

if [ "$RC" -ne 0 ]; then
    grep -E '^pan:[0-9]*:' "$WORK/pan.log" | head -5
fi

exit $RC
