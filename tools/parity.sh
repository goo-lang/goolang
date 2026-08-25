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

case "${1:-}" in
    --list-make-gates)
        list_make_gates
        exit $?
        ;;
    *)
        echo "usage: parity.sh --list-make-gates" >&2
        exit 2
        ;;
esac
