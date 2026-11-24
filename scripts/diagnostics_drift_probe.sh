#!/usr/bin/env bash
# diagnostics-drift-probe — catalogue/diagnostics.tsv must equal what
# scripts/extract_diagnostics.py generates from the tree. The same shape as
# stdlib_coverage_drift.sh: a committed table that quietly falls behind the
# source is worse than no table.
# Teeth: --self-test appends a fake row to a copy and expects a diff.
set -u
cd "$(dirname "$0")/.."
if [ "${1:-}" = "--self-test" ]; then
    tmp=$(mktemp)
    python3 scripts/extract_diagnostics.py > "$tmp"
    printf 'deadbeef\tfake.c\tthis row does not exist\n' >> "$tmp"
    if diff -q "$tmp" <(python3 scripts/extract_diagnostics.py) >/dev/null; then echo "diagnostics-drift-probe --self-test: FAIL (an extra row went unnoticed)"; exit 1; fi
    rm -f "$tmp"
    echo "diagnostics-drift-probe --self-test: PASS (an extra row is a diff)"; exit 0
fi
out=$(mktemp)
if diff -u catalogue/diagnostics.tsv <(python3 scripts/extract_diagnostics.py) > "$out"; then
    echo "diagnostics-drift-probe: PASS ($(($(wc -l < catalogue/diagnostics.tsv) - 1)) diagnostics, catalogue matches the tree)"; rm -f "$out"
else
    echo "diagnostics-drift-probe: FAIL (catalogue/diagnostics.tsv is stale; regenerate with scripts/extract_diagnostics.py)"; head -20 "$out"; rm -f "$out"; exit 1
fi
