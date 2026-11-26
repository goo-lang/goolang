#!/usr/bin/env bash
# Gate parity between the Makefile and Bazel.
#
# The Makefile is this project's gate net: VERIFY_ALL_DEPS names every gate and
# verify-core runs all but the three in HEAVY_DEPS. The exact count moves with
# the branch, so it lives in tools/parity-gate-count.txt rather than here.
# The migration deletes the Makefile in phase 7, and
# this script is the only thing standing between "deleted" and "silently lost
# a gate". It must reach zero unmapped before the Makefile may be removed.
#
# Exit codes:
#   0  every gate has a Bazel counterpart
#   1  a gate is unmapped
#   2  a tool failed (could not read the Makefile, bazel query failed)
#
# Exit status is never read through a pipe: a pipeline reports only its last
# stage's status, which would hide a red result entirely.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MAKEFILE="${MAKEFILE:-$root/Makefile}"

# Reads the VERIFY_ALL_DEPS variable specifically, NOT every *-probe: target.
# The distinction is load-bearing: m12-probe is defined at Makefile:3101 and is
# absent from VERIFY_ALL_DEPS, so it never runs. Scraping target definitions
# would report it as a gate needing migration.
# Same reason as tools/probe_census.sh: sort collation is locale-dependent, and
# this output orders a committed file downstream.
export LC_ALL=C

list_make_gates() {
    if [ ! -r "$MAKEFILE" ]; then
        echo "parity: cannot read $MAKEFILE" >&2
        return 2
    fi
    awk '/^VERIFY_ALL_DEPS[[:space:]]*:?=/{f=1} f{print} f && !/\\[[:space:]]*$/{exit}' "$MAKEFILE" \
        | sed 's/VERIFY_ALL_DEPS[[:space:]]*:\?=//' \
        | tr -d '\\' \
        | tr ' ' '\n' \
        | grep -E '^[a-zA-Z0-9_-]+$' \
        | sort -u
}

# Every test target Bazel knows about, as a bare target name.
#
# PARITY_BAZEL_TESTS overrides the query with a file of target names. This is
# not a convenience: tools/parity_test.sh runs INSIDE a Bazel sandbox, and a
# nested bazel query contends on the output-base lock rather than returning.
# The override lets the mapping logic be tested hermetically. The real query
# path runs from tools/parity_selftest.sh, which is tagged no-sandbox.
list_bazel_tests() {
    if [ -n "${PARITY_BAZEL_TESTS:-}" ]; then
        if [ ! -r "$PARITY_BAZEL_TESTS" ]; then
            echo "parity: cannot read PARITY_BAZEL_TESTS=$PARITY_BAZEL_TESTS" >&2
            return 2
        fi
        sort -u < "$PARITY_BAZEL_TESTS"
        return 0
    fi
    local out
    out="$("${BAZEL:-bazel}" query 'tests(//...)' --output label 2>/dev/null)"
    if [ $? -ne 0 ]; then
        echo "parity: bazel query failed" >&2
        return 2
    fi
    printf '%s\n' "$out" | sed 's|.*:||' | sort -u
}

# switch-probe is claimed by a target named switch_probe.
gate_to_target() {
    printf '%s\n' "$1" | tr '-' '_'
}

ALLOWLIST="${PARITY_ALLOWLIST:-$root/tools/parity-allowlist.txt}"

# Prints the allowlisted gate names. Refuses an entry with no reason: a bare
# gate name would silently drop a gate from the count, which is the one thing
# this whole script exists to prevent.
list_allowlisted() {
    [ -r "$ALLOWLIST" ] || return 0
    local line name rest
    while IFS= read -r line; do
        case "$line" in ''|'#'*) continue ;; esac
        name="${line%% *}"
        rest="${line#"$name"}"
        rest="${rest# }"
        if [ -z "$rest" ]; then
            echo "parity: allowlist entry '$name' has no reason" >&2
            return 2
        fi
        printf '%s\n' "$name"
    done < "$ALLOWLIST"
}

report() {
    local gates targets allowed unmapped=() mapped=0
    gates="$(list_make_gates)" || return 2
    targets="$(list_bazel_tests)" || return 2
    allowed="$(list_allowlisted)" || return 2

    while IFS= read -r gate; do
        [ -z "$gate" ] && continue
        if printf '%s\n' "$targets" | grep -qx "$(gate_to_target "$gate")"; then
            mapped=$((mapped + 1))
        elif printf '%s\n' "$allowed" | grep -qx "$gate"; then
            mapped=$((mapped + 1))
        else
            unmapped+=("$gate")
        fi
    done <<< "$gates"

    echo "make gates: $(printf '%s\n' "$gates" | grep -c .)"
    echo "mapped:     $mapped"
    echo "unmapped:   ${#unmapped[@]}"

    if [ "${#unmapped[@]}" -gt 0 ]; then
        echo
        echo "UNMAPPED (${#unmapped[@]}):"
        printf '  %s\n' "${unmapped[@]}"
        echo
        echo "parity: ${#unmapped[@]} gates have no Bazel test"
        return 1
    fi
    echo
    echo "parity: every gate has a Bazel test"
    return 0
}

case "${1:-}" in
    --list-make-gates)  list_make_gates; exit $? ;;
    --list-bazel-tests) list_bazel_tests; exit $? ;;
    "")                 report; exit $? ;;
    *)
        echo "usage: parity.sh [--list-make-gates|--list-bazel-tests]" >&2
        exit 2
        ;;
esac
