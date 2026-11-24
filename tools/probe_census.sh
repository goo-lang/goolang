#!/usr/bin/env bash
# Classify every *-probe gate in VERIFY_ALL_DEPS by the SHAPE of its recipe.
#
# WHY THIS EXISTS. The spec once said the inline probes "follow one repeated
# shape: compile a fixture, run it, diff against .expected.txt". That was
# measured by asking whether a recipe mentions scripts/ -- a proxy for the
# shape rather than the shape. Measured properly there are five groups, and
# the largest of them is not that shape at all.
#
# Phase 4 reads this file to decide which macro each probe gets, so it is
# generated rather than hand-maintained, and tests/probes/census_current.sh
# gates that the committed copy is current.
#
# Output: one "<category> <gate>" line per probe, sorted, then a summary.
#
# LC_ALL=C is LOAD-BEARING. sort's collation is locale-dependent, and this
# output is a COMMITTED file that census_current.sh diffs. Under en_AU.UTF-8 a
# hyphen is ignored in collation, so "mapkey-reject-probe" sorts before
# "map-nilfunc-abort-probe"; under C it sorts after. The census was therefore
# machine-dependent, and CI disagreed with the development box. Nothing caught
# it because census_current.sh had never run.
export LC_ALL=C
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

gates="$(./tools/parity.sh --list-make-gates)" || { echo "census: cannot read gates" >&2; exit 2; }

recipe() {
    awk -v p="^$1:" '$0~p{f=1;next} f&&/^[^\t]/{exit} f' Makefile
}

# The classification is written to a temp file and sorted afterwards rather
# than piped into `sort`. A `while ... done | sort` runs the loop in a SUBSHELL,
# so the counter below is lost and the empty-corpus guard reads 0 every time --
# it fired on the first run here while the output was perfectly correct.
out="$(mktemp)"
trap 'rm -f "$out"' EXIT

n=0
while IFS= read -r g; do
    case "$g" in *-probe) ;; *) continue ;; esac
    n=$((n + 1))
    body="$(recipe "$g")"
    if printf '%s\n' "$body" | grep -q 'scripts/'; then
        echo "script $g" >> "$out"; continue
    fi
    src="$(printf '%s\n' "$body" | grep -oE 'examples/[a-z0-9_]+\.goo' | head -1)"
    if [ -n "$src" ]; then
        if [ -f "${src%.goo}.expected.txt" ]; then echo "golden $g" >> "$out"; else echo "example $g" >> "$out"; fi
        continue
    fi
    if printf '%s\n' "$body" | grep -qE 'which (valgrind|clang)|for name in|for f in|_NAMES\)'; then
        echo "bespoke $g" >> "$out"
    elif printf '%s\n' "$body" | grep -q 'printf'; then
        echo "printf $g" >> "$out"
    else
        echo "bespoke $g" >> "$out"
    fi
done <<< "$gates" > "$out"

if [ "$n" -eq 0 ]; then
    echo "census: TOOL FAILURE no probe gates found" >&2
    exit 2
fi

sort "$out"
