#!/usr/bin/env bash
# How many heap blocks does bench/daemon/daemon.goo hold at exit?
#
# The daemon is the program ADR 0002 measures ARC against: a loop that
# allocates per request and keeps nothing. Every reclamation the release plan
# gains or loses shows up here first, and until this gate nothing pinned it.
# PR #376 moved it from 4,003 to 10,003 blocks for 2,000 requests (each
# request's `parts` left 3 replaced arrays) and no gate noticed; the figure
# was only in a commit message.
#
# THE BLOCK COUNT, NOT THE BYTES. `main` reads os.Args and the runtime copies
# argv[0] to the heap unfreed, so the byte total moves with the length of the
# output path. The block count does not (CLAUDE.md, "Memory model").
#
# BUDGET_BLOCKS is the measured value plus at most 1%, rounded down. Change it
# in the commit that changes the measurement, and give the reason.
#
#   4,043 (fix/append-alias-sound, 2026-11-27): 4,003 measured, + 1%. valgrind
#         reports 4,000 of them definitely lost (2 for each request, the
#         residual CLAUDE.md names). 719a1c58, which freed no replaced array,
#         measured 10,003.
#
# The run is also an ARC soundness check: valgrind must report no invalid
# access, and the output must be the daemon's.
#
# Usage: scripts/arc_daemon_blocks_probe.sh [--self-test]

set -uo pipefail

case "${COMPILER:-}" in
    "") ;;
    /*) ;;
    *) COMPILER="$PWD/$COMPILER" ;;
esac

ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || ROOT="$PWD"
cd "$ROOT" || exit 2

COMPILER="${COMPILER:-$ROOT/bin/goo}"
export GOOROOT="${GOOROOT:-$ROOT}"
SRC="bench/daemon/daemon.goo"
REQUESTS=2000
BUDGET_BLOCKS=4043
# len(last) for the fixed request in daemon.goo: "21:5:1|2|3|four|5|six|7".
EXPECTED_OUT=23
VG_EXIT=99

SCRATCH=""
cleanup() { [ -n "$SCRATCH" ] && rm -rf "$SCRATCH"; }
trap cleanup EXIT

# The N of valgrind's "in use at exit: X bytes in N blocks", or nothing when
# the line is absent. An absent line must never read as zero blocks.
in_use_blocks() {
    sed -n 's/^==[0-9]*==\s*in use at exit: [0-9,]* bytes in \([0-9,]*\) blocks$/\1/p' "$1" \
        | tr -d ','
}

# Run daemon binary $1 under valgrind, with work files in directory $2. Prints
# one result line and returns 1 on failure.
check_run() {
    local bin="$1" work="$2"
    # Leaks are what this gate COUNTS, so they are not errors here: the exit
    # status speaks only for invalid accesses.
    timeout 300 valgrind --leak-check=full --errors-for-leak-kinds=none \
        --error-exitcode=$VG_EXIT "$bin" "$REQUESTS" > "$work/stdout" 2> "$work/vg.err"
    local rc=$?
    if [ "$rc" -eq "$VG_EXIT" ]; then
        echo "  FAIL (valgrind reported errors)"; grep -m5 -E "Invalid|uninitialised" "$work/vg.err"
        return 1
    fi
    if [ "$rc" -ne 0 ]; then
        echo "  FAIL (daemon exit $rc)"; tail -20 "$work/vg.err"
        return 1
    fi
    local out
    out="$(cat "$work/stdout")"
    if [ "$out" != "$EXPECTED_OUT" ]; then
        echo "  FAIL (output '$out', expected '$EXPECTED_OUT')"
        return 1
    fi
    local blocks
    blocks="$(in_use_blocks "$work/vg.err")"
    if [ -z "$blocks" ]; then
        echo "  FAIL (no 'in use at exit' line in the valgrind log)"
        return 1
    fi
    if [ "$blocks" -gt "$BUDGET_BLOCKS" ]; then
        echo "  FAIL (over budget: $blocks blocks in use at exit, budget $BUDGET_BLOCKS)"
        return 1
    fi
    echo "  ok: $blocks blocks in use at exit for $REQUESTS requests (budget $BUDGET_BLOCKS)"
    return 0
}

# Compile the daemon to $1. Extra leading arguments are environment settings.
build() {
    local out="$1"; shift
    env -u GOO_ARC_RELEASE -u GOO_ARC_DEBUG "$@" "$COMPILER" -o "$out" "$SRC" \
        > "$out.cerr" 2>&1
}

self_test() {
    # Own PATH: a tool appearing or vanishing on the machine must not flip
    # the verdict.
    export PATH=/usr/local/bin:/usr/bin:/bin

    # The parser must not read an absent line as a count, and must read the
    # comma form valgrind prints.
    printf '==1==     in use at exit: 82,198 bytes in 4,003 blocks\n' > "$SCRATCH/log_ok"
    printf '==1== All heap blocks were freed -- no leaks are possible\n' > "$SCRATCH/log_none"
    [ "$(in_use_blocks "$SCRATCH/log_ok")" = "4003" ] \
        || { echo "arc-daemon-blocks-probe self-test: FAIL (cannot parse '4,003 blocks')"; return 1; }
    [ -z "$(in_use_blocks "$SCRATCH/log_none")" ] \
        || { echo "arc-daemon-blocks-probe self-test: FAIL (read a count from a log with none)"; return 1; }

    # THE FAULT: release off. It holds every block ARC would free, so it must
    # be over budget -- and for THAT reason, not because it crashed, or the
    # self-test would pass on a broken build.
    mkdir "$SCRATCH/off" "$SCRATCH/on"
    build "$SCRATCH/off/daemon" GOO_ARC_RELEASE=0 \
        || { echo "arc-daemon-blocks-probe self-test: FAIL (compile, release off)"; cat "$SCRATCH/off/daemon.cerr"; return 1; }
    check_run "$SCRATCH/off/daemon" "$SCRATCH/off" > "$SCRATCH/off/result"
    if ! grep -q "over budget" "$SCRATCH/off/result"; then
        echo "arc-daemon-blocks-probe self-test: FAIL (GOO_ARC_RELEASE=0 was not reported over budget)"
        cat "$SCRATCH/off/result"
        return 1
    fi

    # The control: the ordinary build must pass, or the fault proves nothing.
    build "$SCRATCH/on/daemon" \
        || { echo "arc-daemon-blocks-probe self-test: FAIL (compile, release on)"; cat "$SCRATCH/on/daemon.cerr"; return 1; }
    check_run "$SCRATCH/on/daemon" "$SCRATCH/on" > "$SCRATCH/on/result" \
        || { echo "arc-daemon-blocks-probe self-test: FAIL (the default build failed)"; cat "$SCRATCH/on/result"; return 1; }

    echo "    ok: release off $(sed 's/^  //' "$SCRATCH/off/result")"
    echo "    ok: release on $(sed 's/^  //' "$SCRATCH/on/result")"
    echo "arc-daemon-blocks-probe self-test: PASS"
}

if ! command -v valgrind >/dev/null 2>&1; then
    # Under make, tests.yml greps verify-core.log for the SKIPPED line and
    # fails the job. Under Bazel nobody reads the log, so the skip is a FAIL.
    if [ "${GOO_PROBE_NO_SKIP:-0}" = 1 ]; then
        echo "arc-daemon-blocks-probe: FAIL (valgrind not found, and GOO_PROBE_NO_SKIP forbids a skip)"
        exit 1
    fi
    echo "arc-daemon-blocks-probe: SKIPPED (valgrind not found)"
    exit 0
fi

[ -x "$COMPILER" ] || { echo "arc-daemon-blocks-probe: FAIL (no $COMPILER)"; exit 1; }
[ -f "$SRC" ] || { echo "arc-daemon-blocks-probe: FAIL (no $SRC)"; exit 1; }
SCRATCH="$(mktemp -d)" || { echo "arc-daemon-blocks-probe: FAIL (mktemp)"; exit 1; }

if [ "${1:-}" = "--self-test" ]; then
    self_test
    exit $?
fi

echo "=== arc-daemon-blocks-probe: bench/daemon blocks in use at exit ==="
build "$SCRATCH/daemon" \
    || { echo "  FAIL (compile)"; cat "$SCRATCH/daemon.cerr"; echo "arc-daemon-blocks-probe: FAIL"; exit 1; }
if ! check_run "$SCRATCH/daemon" "$SCRATCH"; then
    echo "arc-daemon-blocks-probe: FAIL"
    exit 1
fi
echo "arc-daemon-blocks-probe: PASS"
