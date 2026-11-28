#!/usr/bin/env bash
# no-skip-probe: a gate whose tool is missing must FAIL, never pass.
#
# WHY THIS EXISTS. Fifteen verify-core gates printed SKIPPED and exited 0 when
# their tool (valgrind, clang, go, ...) was absent. A gate that tested nothing
# then read as green. The rule is GOO_PROBE_NO_SKIP=1 turns every skip into a
# FAIL (the convention at scripts/arc_reassign_probe.sh), and the verify*
# targets export it. This probe proves the rule holds for each of the 15 gates.
#
# HOW. For each gate it runs the REAL make target with GOO_PROBE_NO_SKIP=1 and
# a scrubbed PATH that holds symlinks to every tool the gates need EXCEPT the
# one under test. The gate must exit non-zero AND print a line with FAIL. A
# gate that exits 0, or exits non-zero with no FAIL line that names
# GOO_PROBE_NO_SKIP (a crash is not a verdict), is reported. A recipe that
# prints SKIPPED and carries on fails by accident with FAIL lines of its own,
# and the GOO_PROBE_NO_SKIP requirement is what catches that.
#
# The scrubbed PATH is built from fixed directories, never from the caller's
# PATH, so a tool that appears or vanishes on the machine cannot change the
# verdict. `make -o` marks the compiler and runtime archive as up to date so
# the removed tool, not a rebuild, is what the gate meets first.
#
# Usage: no_skip_probe.sh [--self-test]
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROBE="no-skip-probe"
SLOW_SECONDS=30

# Directories searched for tools, in order. ccache's wrapper directory is left
# out on purpose: its cc/clang links need the caller's PATH to find the real
# compiler.
TOOL_DIRS="/usr/local/bin /usr/bin /bin /usr/local/go/bin"

# Tools every gate recipe may use, other than the tool under test.
BASE_TOOLS="sh bash env make which mkdir cat rm mktemp dirname basename grep egrep sed awk
 tr sort uniq head tail wc cp mv ls find touch date git python3 diff cmp ar ranlib cc gcc
 time id uname readlink realpath sleep true false tee xargs cut chmod ln nproc printf test
 expr timeout valgrind clang go ld as nm"

# gate|tool removed|extra environment ("-" for none)|command
# The command runs from the repository root.
GATE_TABLE="arc-reassign-probe|valgrind|-|make --no-print-directory -s -o bin/goo -o lib/libgoo_runtime.a arc-reassign-probe
arc-map-key-local-probe|valgrind|-|make --no-print-directory -s -o bin/goo -o lib/libgoo_runtime.a arc-map-key-local-probe
arc-multi-assign-probe|valgrind|-|make --no-print-directory -s -o bin/goo -o lib/libgoo_runtime.a arc-multi-assign-probe
arc-uaf-probe|valgrind|-|make --no-print-directory -s -o bin/goo -o lib/libgoo_runtime.a arc-uaf-probe
arc-uaf-selftest|valgrind|-|make --no-print-directory -s arc-uaf-selftest
arc-loop-carried-probe|valgrind|-|make --no-print-directory -s -o bin/goo -o lib/libgoo_runtime.a arc-loop-carried-probe
arc-concat-operand-probe|valgrind|-|make --no-print-directory -s -o bin/goo -o lib/libgoo_runtime.a arc-concat-operand-probe
arc-release-probe|valgrind|-|make --no-print-directory -s -o bin/goo -o lib/libgoo_runtime.a arc-release-probe
arena-valgrind-probe|valgrind|-|make --no-print-directory -s -o bin/goo -o lib/libgoo_runtime.a arena-valgrind-probe
ast-free-leak-probe|valgrind|-|make --no-print-directory -s ast-free-leak-probe
ast-free-leak-selftest|valgrind|-|make --no-print-directory -s ast-free-leak-selftest
obj-header-tsan|clang|-|make --no-print-directory -s obj-header-tsan
stdlib-coverage-drift|go|-|make --no-print-directory -s stdlib-coverage-drift
arena-rss-probe|time|GOO_TIME_BIN=/nonexistent/time|make --no-print-directory -s -o bin/goo -o lib/libgoo_runtime.a arena-rss-probe
archive-determinism-probe|ar|-|make --no-print-directory -s -o lib/libgoo_runtime.a archive-determinism-probe"

# build_path DIR TOOL: fill DIR with symlinks to BASE_TOOLS, omitting TOOL.
build_path() {
    local dir="$1" omit="$2" t d
    for t in $BASE_TOOLS; do
        [ "$t" = "$omit" ] && continue
        for d in $TOOL_DIRS; do
            if [ -x "$d/$t" ] && [ ! -d "$d/$t" ]; then
                ln -s "$d/$t" "$dir/$t"
                break
            fi
        done
    done
}

# run_table TABLE ROOTDIR: run every row. Prints one line per gate. Returns 0
# when every gate failed as required, 1 when any did not, and 2 for an empty
# table (nothing was tested, so it is not a pass).
run_table() {
    local table="$1" rootdir="$2" bad=0 n=0 rows
    local gate tool envs cmd work out rc start elapsed
    work="$(mktemp -d)" || { echo "$PROBE: FAIL (mktemp)"; return 1; }
    while IFS='|' read -r gate tool envs cmd; do
        [ -n "$gate" ] || continue
        n=$((n + 1))
        mkdir "$work/bin.$n"
        build_path "$work/bin.$n" "$tool"
        if [ -e "$work/bin.$n/$tool" ]; then
            echo "  $gate: FAIL (probe could not remove $tool from the scrubbed PATH)"
            bad=$((bad + 1))
            continue
        fi
        local -a extra=()
        [ "$envs" = "-" ] || extra=("$envs")
        start=$SECONDS
        out="$(cd "$rootdir" && env -i PATH="$work/bin.$n" HOME="$work" \
            GOO_PROBE_NO_SKIP=1 "${extra[@]}" bash -c "timeout 120 $cmd" 2>&1 </dev/null)"
        rc=$?
        elapsed=$((SECONDS - start))
        [ -z "${NO_SKIP_PROBE_VERBOSE:-}" ] || printf '%s\n' "$out" | sed 's/^/      | /'
        if [ "$rc" -eq 0 ]; then
            echo "  $gate: FAIL (exit 0 without $tool: the gate skips instead of failing)"
            bad=$((bad + 1))
        elif ! printf '%s\n' "$out" | grep 'FAIL' | grep -q 'GOO_PROBE_NO_SKIP'; then
            echo "  $gate: FAIL (exit $rc without $tool, but no FAIL line that names GOO_PROBE_NO_SKIP: a crash is not a verdict)"
            printf '%s\n' "$out" | tail -3 | sed 's/^/      /'
            bad=$((bad + 1))
        else
            echo "  $gate: ok (exit $rc without $tool, ${elapsed}s)"
        fi
        if [ "$elapsed" -gt "$SLOW_SECONDS" ]; then
            echo "  $gate: CONCERN (took ${elapsed}s: the gate works before its tool check)"
        fi
    done <<< "$table"
    rm -rf "$work"
    if [ "$n" -eq 0 ]; then
        echo "$PROBE: FAIL (the gate table is empty: nothing was tested)"
        return 2
    fi
    # A command that read the loop's stdin would swallow rows. Count them.
    rows="$(printf '%s\n' "$table" | grep -c .)"
    if [ "$n" -ne "$rows" ]; then
        echo "$PROBE: FAIL (processed $n of $rows table rows)"
        return 1
    fi
    if [ "$bad" -ne 0 ]; then
        echo "$PROBE: FAIL ($bad gate(s) did not fail on a missing tool)"
        return 1
    fi
    return 0
}

self_test() {
    local fx rc
    fx="$(mktemp -d)" || { echo "$PROBE self-test: FAIL (mktemp)"; return 1; }
    # The fixture tool name is never on the scrubbed PATH, so removing it is a
    # no-op: the fixtures stand for a gate that already lacks its tool.
    printf '%s\n' 'echo "valgrind not found - SKIPPED"; exit 0' > "$fx/skips.sh"
    printf '%s\n' 'echo "gate: FAIL (tool not found, and GOO_PROBE_NO_SKIP forbids a skip)"; exit 1' > "$fx/fails.sh"
    printf '%s\n' 'exit 3' > "$fx/crashes.sh"
    printf '%s\n' 'cat >/dev/null; echo "gate: FAIL (GOO_PROBE_NO_SKIP)"; exit 1' > "$fx/reads.sh"
    printf '%s\n' 'echo "gate: FAIL (something else broke)"; exit 1' > "$fx/other.sh"

    # 1. A skipping gate must be reported.
    run_table "skipper|faketool|-|sh $fx/skips.sh" "$fx" > "$fx/o1" 2>&1
    rc=$?
    if [ "$rc" -ne 1 ] || ! grep -q 'skipper: FAIL' "$fx/o1"; then
        echo "$PROBE self-test: FAIL (a gate that exits 0 was not reported)"; cat "$fx/o1"
        rm -rf "$fx"; return 1
    fi
    # 2. A failing gate must pass.
    run_table "failer|faketool|-|sh $fx/fails.sh" "$fx" > "$fx/o2" 2>&1
    rc=$?
    if [ "$rc" -ne 0 ] || ! grep -q 'failer: ok' "$fx/o2"; then
        echo "$PROBE self-test: FAIL (a gate that fails with FAIL was not accepted)"; cat "$fx/o2"
        rm -rf "$fx"; return 1
    fi
    # 3. A crash with no FAIL line must be reported.
    run_table "crasher|faketool|-|sh $fx/crashes.sh" "$fx" > "$fx/o3" 2>&1
    rc=$?
    if [ "$rc" -ne 1 ] || ! grep -q 'crasher: FAIL' "$fx/o3"; then
        echo "$PROBE self-test: FAIL (a crash with no FAIL line was not reported)"; cat "$fx/o3"
        rm -rf "$fx"; return 1
    fi
    # 3b. A failure that does not name the skip rule must be reported.
    run_table "other|faketool|-|sh $fx/other.sh" "$fx" > "$fx/o3b" 2>&1
    rc=$?
    if [ "$rc" -ne 1 ] || ! grep -q 'other: FAIL' "$fx/o3b"; then
        echo "$PROBE self-test: FAIL (a failure unrelated to the skip rule was accepted)"; cat "$fx/o3b"
        rm -rf "$fx"; return 1
    fi
    # 4. An empty table must exit 2.
    run_table "" "$fx" > "$fx/o4" 2>&1
    rc=$?
    if [ "$rc" -ne 2 ]; then
        echo "$PROBE self-test: FAIL (an empty table exited $rc, not 2)"; cat "$fx/o4"
        rm -rf "$fx"; return 1
    fi
    # 4b. A gate that reads stdin must not swallow the rows after it.
    run_table "reader|faketool|-|sh $fx/reads.sh
second|faketool|-|sh $fx/fails.sh" "$fx" > "$fx/o4b" 2>&1
    rc=$?
    if [ "$rc" -ne 0 ] || ! grep -q 'second: ok' "$fx/o4b"; then
        echo "$PROBE self-test: FAIL (a gate that reads stdin swallowed a row)"; cat "$fx/o4b"
        rm -rf "$fx"; return 1
    fi
    # 5. The tool under test must really be absent from the scrubbed PATH.
    mkdir "$fx/p"
    build_path "$fx/p" "ar"
    if [ -e "$fx/p/ar" ] || [ ! -e "$fx/p/make" ]; then
        echo "$PROBE self-test: FAIL (build_path did not omit ar, or dropped make)"
        rm -rf "$fx"; return 1
    fi
    rm -rf "$fx"
    echo "$PROBE self-test: PASS (skip reported, fail accepted, crash reported, empty table exits 2)"
}

if [ "${1:-}" = "--self-test" ]; then
    self_test
    exit $?
fi

echo "=== $PROBE: each gate must FAIL when its tool is missing ==="
run_table "$GATE_TABLE" "$ROOT"
rc=$?
[ "$rc" -eq 0 ] || exit "$rc"
echo "$PROBE: PASS (all 15 gates fail when their tool is missing)"
