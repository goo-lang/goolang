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

# The mapping logic, exercised against an INJECTED target list. bazel is never
# invoked here: this script runs inside a Bazel sandbox and a nested query
# would contend on the output-base lock rather than returning.
tmp_targets="$(mktemp)"
printf 'm10_probe\nswitch_probe\n' > "$tmp_targets"

# PARITY_ALLOWLIST=/dev/null keeps the real tools/parity-allowlist.txt out of
# these counts. Otherwise a gate added to that file would move the expected
# mapped and unmapped totals.
report="$(PARITY_BAZEL_TESTS="$tmp_targets" PARITY_ALLOWLIST=/dev/null ./tools/parity.sh 2>&1)"
rc=$?
if [ "$rc" -ne 1 ]; then
    echo "parity_test: FAIL report exited $rc, expected 1 (gates remain)"
    fail=1
fi

m="$(printf '%s\n' "$report" | sed -n 's/^make gates:[[:space:]]*//p')"
u="$(printf '%s\n' "$report" | sed -n 's/^unmapped:[[:space:]]*//p')"
mapped="$(printf '%s\n' "$report" | sed -n 's/^mapped:[[:space:]]*//p')"

# Exactly two gates were offered a counterpart, so exactly two must map.
if [ "$mapped" -ne 2 ]; then
    echo "parity_test: FAIL injected 2 targets, mapped $mapped"
    fail=1
fi
if [ "$((m - mapped))" -ne "$u" ]; then
    echo "parity_test: FAIL arithmetic: $m - $mapped != $u"
    fail=1
fi
rm -f "$tmp_targets"

# An allowlisted gate must leave the unmapped count, and an entry with no
# reason must be refused rather than silently accepted.
tmp_allow="$(mktemp)"
tmp_empty="$(mktemp)"
: > "$tmp_empty"   # no Bazel targets at all, so only the allowlist can move it

# A gate NOT mapped by the fixture above, so the delta is attributable to the
# allowlist alone.
echo "enum-probe covered elsewhere, see spec 4.7" > "$tmp_allow"
base_u="$(PARITY_BAZEL_TESTS="$tmp_empty" PARITY_ALLOWLIST=/dev/null ./tools/parity.sh 2>/dev/null | sed -n 's/^unmapped:[[:space:]]*//p')"
allow_u="$(PARITY_BAZEL_TESTS="$tmp_empty" PARITY_ALLOWLIST="$tmp_allow" ./tools/parity.sh 2>/dev/null | sed -n 's/^unmapped:[[:space:]]*//p')"
if [ "$((base_u - allow_u))" -ne 1 ]; then
    echo "parity_test: FAIL allowlisting one gate moved unmapped $base_u -> $allow_u (want -1)"
    fail=1
fi

echo "enum-probe" > "$tmp_allow"
PARITY_BAZEL_TESTS="$tmp_empty" PARITY_ALLOWLIST="$tmp_allow" ./tools/parity.sh >/dev/null 2>&1
if [ $? -ne 2 ]; then
    echo "parity_test: FAIL an allowlist entry with no reason was accepted"
    fail=1
fi
rm -f "$tmp_allow" "$tmp_empty"

# Whitespace. Make accepts a TAB as a line indent, and test-golden-poison sits
# on a TAB-led line of VERIFY_ALL_DEPS (Makefile:3498). A reader that splits on
# spaces alone drops that gate without any error. The fixture has three gates,
# the middle one on a TAB-led continuation line, and all three must be listed.
tmp_make="$(mktemp)"
printf 'VERIFY_ALL_DEPS = alpha-probe \\\n\tbeta-probe \\\n\tgamma-probe\n' > "$tmp_make"
fixture_gates="$(MAKEFILE="$tmp_make" ./tools/parity.sh --list-make-gates 2>/dev/null)"
fixture_count="$(printf '%s\n' "$fixture_gates" | grep -c .)"
if [ "$fixture_count" -ne 3 ]; then
    echo "parity_test: FAIL TAB-led gate line: read $fixture_count gates, want 3"
    fail=1
fi
for want in alpha-probe beta-probe gamma-probe; do
    if ! printf '%s\n' "$fixture_gates" | grep -qx "$want"; then
        echo "parity_test: FAIL TAB-led gate line: $want missing"
        fail=1
    fi
done
rm -f "$tmp_make"

if [ "$fail" -eq 0 ]; then
    echo "parity_test: PASS $count gates (baseline $expected), mapping, arithmetic and allowlist correct"
fi
exit "$fail"
