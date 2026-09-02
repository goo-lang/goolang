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
#   CC        C compiler (default cc)
#   KEEP_DIR  directory to copy the model, pan sources and any .trail into
#             (the work directory itself is always a fresh mktemp -d and is
#             removed on exit, so pan.* files never land in the tree)
#
# Exit status:
#   0  pan reported 0 errors
#   1  pan reported at least one error (assertion violation or invalid end state)
#   2  bad arguments, or spin / cc / pan failed to run

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

if ! ( cd "$WORK" && "$CC" -O2 -DSAFETY -o pan pan.c ) > "$WORK/cc.log" 2>&1; then
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

# pan stops at the first error, so a FAIL state count is what it explored
# before the error, not the whole state space. Say which it is.
COMPLETED=yes
if grep -q 'Search not completed' "$WORK/pan.log"; then
    COMPLETED=no
fi
# A truncated search that found nothing is NOT a pass. pan says
# "error: max search depth too small" on stdout and still prints "errors: 0".
if grep -q 'max search depth too small' "$WORK/pan.log"; then
    COMPLETED=no
    if [ "$ERRORS" -eq 0 ]; then
        echo "run_deadlock_model: INCONCLUSIVE N=$N K=$K DETECTOR=$DETECTOR -- pan hit its search-depth limit and found no error; raise it with PAN_ARGS='-m<depth>'" >&2
        exit 2
    fi
fi

if [ "$ERRORS" -eq 0 ]; then
    VERDICT=PASS
    RC=0
else
    VERDICT=FAIL
    RC=1
fi

echo "run_deadlock_model: $VERDICT model=tests/models/deadlock_detector.pml N=$N K=$K DETECTOR=$DETECTOR errors=$ERRORS invalid_end_states=$INVALID_END states=$STATES search_completed=$COMPLETED depth=$DEPTH wall=${WALL}s spin='$SPIN_VERSION' cc='$($CC --version 2>/dev/null | head -1)'"

if [ "$RC" -ne 0 ]; then
    grep -E '^pan:[0-9]*:' "$WORK/pan.log" | head -5
fi

exit $RC
