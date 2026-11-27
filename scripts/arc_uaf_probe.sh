#!/usr/bin/env bash
# Does ARC release a value while something still reads it (use-after-free)?
#
# A fixture is tests/arc_uaf/<name>.goo plus tests/arc_uaf/<name>.expected.txt.
# The gate passes only if each fixture compiles with default flags (ARC on),
# runs under `valgrind --error-exitcode=99` with exit 0, and prints exactly the
# expected stdout. Tracks 2-4 of the security-audit fixes add their regression
# fixtures here, so this format is an interface.
#
# Every producer is checked before its output is trusted: the compiler status
# before the binary runs, and the binary status before stdout is compared. A
# crashing binary prints nothing, and an empty file equals an empty file.
#
# Usage: scripts/arc_uaf_probe.sh [--self-test]

set -uo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "FATAL: not in a git repository" >&2; exit 2; }
cd "$ROOT" || exit 2

COMPILER="./bin/goo"
FIXTURE_DIR="tests/arc_uaf"
VG_EXIT=99
export GOOROOT="${GOOROOT:-$ROOT}"

# One scratch directory per run, removed on every exit path.
SCRATCH=""
cleanup() { [ -n "$SCRATCH" ] && rm -rf "$SCRATCH"; }
trap cleanup EXIT

# Run binary $1 under valgrind and compare its stdout with file $2.
# Work files go in directory $3. Prints the reason and returns 1 on failure.
check_binary() {
    local bin="$1" expected="$2" work="$3"
    # timeout: a hung fixture must not hang make verify-core.
    timeout 60 valgrind --error-exitcode=$VG_EXIT -q "$bin" \
        > "$work/stdout" 2> "$work/vg.err"
    local rc=$?
    if [ "$rc" -eq 124 ]; then
        echo "  FAIL (timeout)"
        return 1
    fi
    if [ "$rc" -eq "$VG_EXIT" ]; then
        echo "  FAIL (valgrind reported errors)"; head -20 "$work/vg.err"
        return 1
    fi
    if [ "$rc" -ne 0 ]; then
        echo "  FAIL (binary exit $rc)"; head -20 "$work/vg.err"
        return 1
    fi
    if ! diff -u "$expected" "$work/stdout" > "$work/diff"; then
        echo "  FAIL (stdout differs)"; cat "$work/diff"
        return 1
    fi
    return 0
}

self_test() {
    # Own PATH: a tool appearing or vanishing on the machine must not flip
    # the verdict.
    export PATH=/usr/local/bin:/usr/bin:/bin
    local cc="${CC:-cc}"
    cat > "$SCRATCH/bad.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    int *p = malloc(sizeof *p); *p = 7; free(p);
    printf("%d\n", *p);
    return 0;
}
C
    cat > "$SCRATCH/good.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    int *p = malloc(sizeof *p); *p = 7;
    printf("%d\n", *p);
    free(p);
    return 0;
}
C
    echo 7 > "$SCRATCH/expected.txt"
    # -O0 so the C compiler cannot fold the freed read away. $cc is unquoted
    # because CC may carry a launcher word such as ccache.
    # shellcheck disable=SC2086
    $cc -O0 -o "$SCRATCH/bad" "$SCRATCH/bad.c" \
        && $cc -O0 -o "$SCRATCH/good" "$SCRATCH/good.c" \
        || { echo "arc-uaf-probe self-test: FAIL (cannot compile C controls with $cc)"; return 1; }

    mkdir "$SCRATCH/w_good" "$SCRATCH/w_bad"
    check_binary "$SCRATCH/good" "$SCRATCH/expected.txt" "$SCRATCH/w_good" >/dev/null \
        || { echo "arc-uaf-probe self-test: FAIL (correct program reported as failure)"; return 1; }
    if check_binary "$SCRATCH/bad" "$SCRATCH/expected.txt" "$SCRATCH/w_bad" >/dev/null; then
        echo "arc-uaf-probe self-test: FAIL (use-after-free program reported as pass)"
        return 1
    fi
    echo "arc-uaf-probe self-test: PASS"
}

if ! command -v valgrind >/dev/null 2>&1; then
    # A skipped gate tested nothing. The verify* targets set GOO_PROBE_NO_SKIP,
    # and under it the skip is the failure (the arc_reassign_probe.sh rule).
    if [ "${GOO_PROBE_NO_SKIP:-0}" = 1 ]; then
        echo "arc-uaf-probe: FAIL (valgrind not found, and GOO_PROBE_NO_SKIP forbids a skip)"
        exit 1
    fi
    echo "arc-uaf-probe: SKIPPED (valgrind not found)"
    exit 0
fi

SCRATCH="$(mktemp -d)" || { echo "arc-uaf-probe: FAIL (mktemp)"; exit 1; }

if [ "${1:-}" = "--self-test" ]; then
    self_test
    exit $?
fi

[ -x "$COMPILER" ] || { echo "arc-uaf-probe: FAIL (no $COMPILER)"; exit 1; }

shopt -s nullglob
fixtures=("$FIXTURE_DIR"/*.goo)
if [ "${#fixtures[@]}" -eq 0 ]; then
    echo "arc-uaf-probe: FAIL (no fixtures in $FIXTURE_DIR)"
    exit 1
fi

echo "=== arc-uaf-probe: use-after-free fixtures under valgrind ==="
fail=0
for src in "${fixtures[@]}"; do
    name="$(basename "$src" .goo)"
    expected="$FIXTURE_DIR/$name.expected.txt"
    if [ ! -f "$expected" ]; then
        echo "  $name: FAIL (no $expected)"; fail=1; continue
    fi
    mkdir "$SCRATCH/$name"
    # env -u: the ARC kill switch in the caller's environment would turn
    # release off, and the gate would then test nothing.
    env -u GOO_ARC_RELEASE -u GOO_ARC_DEBUG \
        "$COMPILER" -o "$SCRATCH/$name/prog" "$src" > "$SCRATCH/$name/cerr" 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "  $name: FAIL (compile exit $rc)"; cat "$SCRATCH/$name/cerr"
        fail=1; continue
    fi
    if check_binary "$SCRATCH/$name/prog" "$expected" "$SCRATCH/$name"; then
        echo "  $name: ok"
    else
        echo "  $name: FAIL"; fail=1
    fi
done

if [ "$fail" -ne 0 ]; then echo "arc-uaf-probe: FAIL"; exit 1; fi
echo "arc-uaf-probe: PASS"
