#!/usr/bin/env bash
# Gates tools/parity.sh's Makefile reader against facts measured on 2026-08-25.
#
# The three assertions are chosen so that a broken reader cannot pass:
#   - the exact count, so a reader that drops or duplicates entries fails
#   - a gate KNOWN to be present, so an empty read cannot pass  (positive control)
#   - a gate KNOWN to be absent, so a reader that returns everything fails
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

gates="$(./tools/parity.sh --list-make-gates)"
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "parity_test: --list-make-gates exited $rc"
    exit 2
fi

fail=0

# The count is a RECORDED BASELINE, not a constant, because it moves with the
# branch: golden-selftest is in VERIFY_ALL_DEPS on test/golden-runner-teeth
# (5c633f6) and not on main, so main reads 216 and that branch reads 217.
# Same idiom as scripts/grammar-tripwire.sh's EXPECTED_SR and
# scripts/probe-teeth-baseline.txt: a changed count is stop-the-line, and
# bumping it is a deliberate one-line edit rather than silent drift.
expected="$(grep -oE '^[0-9]+' tools/parity-gate-count.txt 2>/dev/null | head -1)"
if [ -z "$expected" ]; then
    echo "parity_test: TOOL FAILURE cannot read tools/parity-gate-count.txt"
    exit 2
fi
count="$(printf '%s\n' "$gates" | grep -c .)"
if [ "$count" -ne "$expected" ]; then
    echo "parity_test: FAIL gate count moved: baseline $expected, read $count"
    echo "  If a gate was added or removed on purpose, update"
    echo "  tools/parity-gate-count.txt in the same commit."
    fail=1
fi

# Positive control. m10-probe is in VERIFY_ALL_DEPS. If this is missing, the
# reader returned nothing usable and the count check above proves nothing.
if ! printf '%s\n' "$gates" | grep -qx 'm10-probe'; then
    echo "parity_test: FAIL m10-probe absent (reader returned nothing usable)"
    fail=1
fi

# Negative control. m12-probe is DEFINED in the Makefile at line 3101 but is
# absent from VERIFY_ALL_DEPS -- it never runs. A reader that scrapes target
# definitions instead of the variable would wrongly include it.
if printf '%s\n' "$gates" | grep -qx 'm12-probe'; then
    echo "parity_test: FAIL m12-probe present (reader scraped targets, not VERIFY_ALL_DEPS)"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "parity_test: PASS $count gates (baseline $expected), controls both correct"
fi
exit "$fail"
