#!/usr/bin/env bash
# Does ARC release a value while something still reads it (use-after-free)?
#
# A fixture is tests/arc_uaf/<name>.goo plus tests/arc_uaf/<name>.expected.txt
# and tests/arc_uaf/<name>.verdict.txt. The gate passes only if each fixture
# compiles with default flags (ARC on), states the release verdicts it expects,
# runs under `valgrind --error-exitcode=99` with exit 0, and prints exactly the
# expected stdout. Tracks 2-4 of the security-audit fixes add their regression
# fixtures here, so this format is an interface.
#
# THE VERDICT FILE is the GOO_ARC_DEBUG=1 lines of the fixture's OWN functions
# (`[arc?] fn: local -> VERDICT` and `[arc] fn: ...`), with the reason set
# stripped. A clean valgrind run proves nothing about a fixture whose locals
# never reach the release path it was written for: a refusal is also clean. So
# each fixture names the verdict it needs, and a change to that verdict is RED
# until a person rewrites the file. Make one with
#     GOO_ARC_DEBUG=1 ./bin/goo -o /dev/null F.goo 2>&1 | scripts/arc_uaf_probe.sh --extract
# and then read it: the file is a claim, not a recording.
#
# <name>.leakcheck, when present, also makes a DEFINITE leak a valgrind error.
# It is opt-in because most programs leak by design (the CLAUDE.md "What is
# NOT reclaimed" list). A fixture that exists to show a release frees
# everything uses it, so removing that release turns the gate RED.
#
# Every producer is checked before its output is trusted: the compiler status
# before the binary runs, and the binary status before stdout is compared. A
# crashing binary prints nothing, and an empty file equals an empty file.
#
# Usage: scripts/arc_uaf_probe.sh [--self-test | --extract]

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

# The verdict lines of a GOO_ARC_DEBUG=1 compile, read from stdin. Vendored
# stdlib functions (goo_pkg__*) are dropped: they are not the fixture's claim,
# and they move whenever goostd/ does. The reason set is dropped because it
# describes WHY, and the gate pins WHAT.
extract_verdicts() {
    grep -E '^\[arc\??\] ' | grep -vE '^\[arc\??\] goo_pkg__' \
        | sed -E 's/ \(reasons=[^)]*\)$//'
}

# Run binary $1 under valgrind and compare its stdout with file $2.
# Work files go in directory $3. $4 = "leak" also fails on a definite leak.
# Prints the reason and returns 1 on failure.
check_binary() {
    local bin="$1" expected="$2" work="$3" leak="${4:-}"
    local vg_flags=(--error-exitcode=$VG_EXIT -q)
    if [ "$leak" = "leak" ]; then
        vg_flags+=(--leak-check=full --errors-for-leak-kinds=definite)
    fi
    # timeout: a hung fixture must not hang make verify-core.
    timeout 60 valgrind "${vg_flags[@]}" "$bin" \
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

# Check fixture $2 in directory $1, with work files in directory $3. Prints one
# result line and returns 1 on failure.
check_fixture() {
    local dir="$1" name="$2" work="$3"
    local src="$dir/$name.goo" expected="$dir/$name.expected.txt"
    local verdicts="$dir/$name.verdict.txt"
    if [ ! -f "$expected" ]; then
        echo "  $name: FAIL (no $expected)"; return 1
    fi
    if [ ! -f "$verdicts" ]; then
        echo "  $name: FAIL (no $verdicts)"; return 1
    fi
    mkdir -p "$work"
    # env -u GOO_ARC_RELEASE: the ARC kill switch in the caller's environment
    # would turn release off, and the gate would then test nothing.
    # GOO_ARC_DEBUG only prints (every reader is a getenv guarding fprintf),
    # so one compile gives both the binary and its verdicts.
    env -u GOO_ARC_RELEASE GOO_ARC_DEBUG=1 \
        "$COMPILER" -o "$work/prog" "$src" > "$work/cerr" 2>&1
    local rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "  $name: FAIL (compile exit $rc)"; cat "$work/cerr"
        return 1
    fi
    extract_verdicts < "$work/cerr" > "$work/verdict"
    if [ ! -s "$work/verdict" ]; then
        # An empty extraction equals an empty file. A fixture with no release
        # candidate at all is not testing ARC.
        echo "  $name: FAIL (no verdict lines in the compiler output)"; return 1
    fi
    if ! diff -u "$verdicts" "$work/verdict" > "$work/verdict.diff"; then
        echo "  $name: FAIL (verdicts differ from $verdicts)"; cat "$work/verdict.diff"
        return 1
    fi
    local leak=""
    [ -f "$dir/$name.leakcheck" ] && leak="leak"
    if check_binary "$work/prog" "$expected" "$work" "$leak"; then
        echo "  $name: ok"
        return 0
    fi
    echo "  $name: FAIL"
    return 1
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

    cat > "$SCRATCH/leak.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    int *p = malloc(sizeof *p); *p = 7;
    printf("%d\n", *p);
    p = NULL;
    return 0;
}
C
    # shellcheck disable=SC2086
    $cc -O0 -o "$SCRATCH/leak" "$SCRATCH/leak.c" \
        || { echo "arc-uaf-probe self-test: FAIL (cannot compile the leak control with $cc)"; return 1; }

    mkdir "$SCRATCH/w_good" "$SCRATCH/w_bad" "$SCRATCH/w_leak" "$SCRATCH/w_leak_off"
    check_binary "$SCRATCH/good" "$SCRATCH/expected.txt" "$SCRATCH/w_good" leak >/dev/null \
        || { echo "arc-uaf-probe self-test: FAIL (correct program reported as failure)"; return 1; }
    if check_binary "$SCRATCH/bad" "$SCRATCH/expected.txt" "$SCRATCH/w_bad" >/dev/null; then
        echo "arc-uaf-probe self-test: FAIL (use-after-free program reported as pass)"
        return 1
    fi
    # The leak check must bite only when asked: on, the leak is RED; off, the
    # same binary is GREEN, which is what every fixture without .leakcheck sees.
    if check_binary "$SCRATCH/leak" "$SCRATCH/expected.txt" "$SCRATCH/w_leak" leak >/dev/null; then
        echo "arc-uaf-probe self-test: FAIL (definite leak reported as pass under .leakcheck)"
        return 1
    fi
    check_binary "$SCRATCH/leak" "$SCRATCH/expected.txt" "$SCRATCH/w_leak_off" >/dev/null \
        || { echo "arc-uaf-probe self-test: FAIL (leak control failed with no .leakcheck)"; return 1; }

    # THE VERDICT FILE. A real fixture, copied, must pass as it stands and go
    # RED when its verdict file is wrong or missing. The wrong file changes ONE
    # verdict, which is the edit a broken release plan would make.
    [ -x "$COMPILER" ] || { echo "arc-uaf-probe self-test: FAIL (no $COMPILER)"; return 1; }
    local ctl="append_owned_release" d="$SCRATCH/fx"
    mkdir "$d"
    for ext in goo expected.txt verdict.txt leakcheck; do
        cp "$FIXTURE_DIR/$ctl.$ext" "$d/$ctl.$ext" \
            || { echo "arc-uaf-probe self-test: FAIL (control fixture $ctl.$ext missing)"; return 1; }
    done
    check_fixture "$d" "$ctl" "$SCRATCH/v_good" >/dev/null \
        || { echo "arc-uaf-probe self-test: FAIL (control fixture $ctl reported as failure)"; return 1; }
    grep -q -- '-> RELEASE_OK$' "$d/$ctl.verdict.txt" \
        || { echo "arc-uaf-probe self-test: FAIL (control verdict file has no RELEASE_OK line)"; return 1; }
    sed -i -E '0,/-> RELEASE_OK$/s//-> RELEASE_NO_ALIASED/' "$d/$ctl.verdict.txt"
    if check_fixture "$d" "$ctl" "$SCRATCH/v_wrong" >/dev/null; then
        echo "arc-uaf-probe self-test: FAIL (a wrong verdict file reported as pass)"
        return 1
    fi
    rm "$d/$ctl.verdict.txt"
    if check_fixture "$d" "$ctl" "$SCRATCH/v_missing" >/dev/null; then
        echo "arc-uaf-probe self-test: FAIL (a missing verdict file reported as pass)"
        return 1
    fi
    echo "arc-uaf-probe self-test: PASS"
}

if [ "${1:-}" = "--extract" ]; then
    extract_verdicts
    exit 0
fi

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
    check_fixture "$FIXTURE_DIR" "$name" "$SCRATCH/$name" || fail=1
done

if [ "$fail" -ne 0 ]; then echo "arc-uaf-probe: FAIL"; exit 1; fi
echo "arc-uaf-probe: PASS"
