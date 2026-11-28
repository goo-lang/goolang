#!/usr/bin/env bash
# Measures the wall time of each verify-core gate, one gate at a time.
#
# Usage:
#   tools/gate_times.sh [--self-test] [gate ...]
#
# With no gate names, the gates are every name from
# `tools/parity.sh --list-make-gates` minus the names in HEAVY_DEPS of the
# Makefile. Each gate runs as `make <gate>` after one `make lexer`. Gates run
# serially. One TSV row per gate is written:
#   gate<TAB>wall_seconds<TAB>exit_status
#
# A gate is a phony Make target, and its phony prerequisites run again inside
# `make <gate>`. So one row can include the time of another gate. The rows
# overlap, and their sum is larger than one full `make verify-core` run.
#
# A failing gate is data. Its exit status goes in its row, and the sweep goes
# on. The script exits 2 only when a tool fails or the gate list is empty.
#
# Environment:
#   MAKEFILE          Makefile to measure (default: the Makefile beside tools/).
#   GATE_TIMES_OUT    File for the TSV rows (default: stdout).
#   GATE_TIMES_LOGDIR Directory for one make log per gate (default: a new
#                     mktemp directory, printed to stderr).
#
# Exit status is never read through a pipe: a pipeline reports only its last
# stage, which would hide a failing make.
set -uo pipefail

self="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
root="$(cd "$(dirname "$self")/.." && pwd)"
MAKEFILE="${MAKEFILE:-$root/Makefile}"
export MAKEFILE

# The gate names in HEAVY_DEPS are excluded from the default gate list. The
# list lives in the Makefile, so this script reads it there rather than keeping
# a second copy that could drift.
heavy_deps() {
    awk '/^HEAVY_DEPS[[:space:]]*:?=/{ sub(/^[^:=]*:?=/, ""); print }' "$MAKEFILE" \
        | tr ' ' '\n' \
        | grep -E '^[a-zA-Z0-9_-]+$'
}

# Prints the default gate list, one name per line. Fails with 2 when the
# parity tool fails or when no gate name remains after the heavy set is
# removed.
default_gates() {
    local all heavy gate
    all="$("$root/tools/parity.sh" --list-make-gates)" || return 2
    heavy="$(heavy_deps)"
    while IFS= read -r gate; do
        [ -z "$gate" ] && continue
        if printf '%s\n' "$heavy" | grep -qxF "$gate"; then
            continue
        fi
        printf '%s\n' "$gate"
    done <<< "$all"
}

# Runs one gate and prints its TSV row. The make status is read directly,
# never through a pipe.
time_gate() {
    local gate="$1" logdir="$2" start end rc secs
    start="$(date +%s.%N)"
    make -f "$MAKEFILE" "$gate" >"$logdir/$gate.log" 2>&1
    rc=$?
    end="$(date +%s.%N)"
    secs="$(awk -v a="$start" -v b="$end" 'BEGIN { printf "%.3f", b - a }')"
    printf '%s\t%s\t%s\n' "$gate" "$secs" "$rc"
}

run_sweep() {
    local out="${GATE_TIMES_OUT:-}" logdir gates gate row
    if [ "$#" -gt 0 ]; then
        gates="$(printf '%s\n' "$@")"
    else
        gates="$(default_gates)" || return 2
    fi
    if [ -z "$(printf '%s' "$gates" | tr -d '[:space:]')" ]; then
        echo "gate_times: the gate list is empty" >&2
        return 2
    fi

    logdir="${GATE_TIMES_LOGDIR:-$(mktemp -d)}" || return 2
    mkdir -p "$logdir" || return 2
    echo "gate_times: make logs in $logdir" >&2

    cd "$root" || return 2
    if ! make -f "$MAKEFILE" lexer >"$logdir/lexer.log" 2>&1; then
        echo "gate_times: make lexer failed, see $logdir/lexer.log" >&2
        return 2
    fi

    while IFS= read -r gate; do
        [ -z "$gate" ] && continue
        row="$(time_gate "$gate" "$logdir")"
        if [ -n "$out" ]; then
            printf '%s\n' "$row" >> "$out" || return 2
        else
            printf '%s\n' "$row"
        fi
    done <<< "$gates"
    return 0
}

# Proves the timer and the exit status capture work, and that an empty gate
# list is refused. PATH is fixed so that no tool found on the machine can
# change the result.
self_test() {
    local scratch fail=0 out rc rows seconds status
    scratch="$(mktemp -d)" || return 2

    cat > "$scratch/Makefile" <<'EOF'
lexer:
	@true
slow:
	sleep 1; true
bad:
	false
EOF
    printf 'HEAVY_DEPS := bad\nVERIFY_ALL_DEPS := \\\n    slow \\\n    bad\n' > "$scratch/heavy.mk"
    # Every gate is heavy, so only the empty-list check can cause exit 2.
    printf 'HEAVY_DEPS := bad\nVERIFY_ALL_DEPS := \\\n    bad\n' > "$scratch/none.mk"

    out="$scratch/rows.tsv"
    env -i PATH=/usr/bin:/bin HOME="$HOME" MAKEFILE="$scratch/Makefile" \
        GATE_TIMES_OUT="$out" GATE_TIMES_LOGDIR="$scratch/logs" \
        bash "$self" slow bad
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "self-test FAIL: sweep exited $rc, expected 0 (a failing gate is data)" >&2
        fail=1
    fi
    rows="$(grep -c . "$out")"
    if [ "$rows" -ne 2 ]; then
        echo "self-test FAIL: expected 2 rows, found $rows" >&2
        fail=1
    fi

    seconds="$(awk -F'\t' 'NR==1 { print $2 }' "$out")"
    if ! awk -v s="$seconds" 'BEGIN { exit !(s >= 1) }'; then
        echo "self-test FAIL: first row seconds '$seconds' is below 1" >&2
        fail=1
    fi

    status="$(awk -F'\t' 'NR==2 { print $3 }' "$out")"
    if [ "$status" = "0" ] || [ -z "$status" ]; then
        echo "self-test FAIL: second row exit_status '$status' is not non-zero" >&2
        fail=1
    fi

    # An empty default gate list must exit 2.
    env -i PATH=/usr/bin:/bin HOME="$HOME" MAKEFILE="$scratch/none.mk" \
        GATE_TIMES_OUT="$scratch/none.tsv" GATE_TIMES_LOGDIR="$scratch/logs" \
        bash "$self" >/dev/null 2>"$scratch/none.err"
    rc=$?
    if [ "$rc" -ne 2 ] || ! grep -q 'the gate list is empty' "$scratch/none.err"; then
        echo "self-test FAIL: empty gate list exited $rc, expected 2 with 'the gate list is empty'" >&2
        fail=1
    fi

    # A heavy gate must be left out of the default list.
    local defaults
    defaults="$(env -i PATH=/usr/bin:/bin HOME="$HOME" MAKEFILE="$scratch/heavy.mk" \
        bash -c "source '$self' --source-only 2>/dev/null; default_gates")"
    if [ "$defaults" != "slow" ]; then
        echo "self-test FAIL: default list with HEAVY_DEPS was '$defaults', expected 'slow'" >&2
        fail=1
    fi

    rm -rf "$scratch"
    if [ "$fail" -ne 0 ]; then
        echo "self-test: FAIL"
        return 1
    fi
    echo "self-test: PASS"
    return 0
}

case "${1:-}" in
    --self-test)
        self_test
        exit $?
        ;;
    # --source-only lets the self-test load the functions without running a sweep.
    --source-only)
        return 0 2>/dev/null || exit 0
        ;;
esac

run_sweep "$@"
exit $?
