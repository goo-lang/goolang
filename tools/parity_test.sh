#!/usr/bin/env bash
# Gates tools/parity.sh's Makefile reader against facts measured on 2026-08-25.
#
# The assertions are chosen so that a broken reader cannot pass:
#   - the sorted list tools/parity-gates.txt, so a dropped, extra or duplicated gate fails
#   - a gate KNOWN to be present, so an empty read cannot pass  (positive control)
#   - a gate KNOWN to be absent, so a reader that returns everything fails
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

# check_list MAKE_GATES_FILE LIST_FILE
# 0 = equal, 1 = differ (each differing gate named), 2 = tool failure.
# The list is a sorted file of gate names rather than a count: two branches
# that each add a different gate add two different lines and merge correctly,
# where a count merges "cleanly" to a wrong number.
check_list() {
    local mk="$1" list="$2" rc=0 g
    if [ ! -r "$list" ]; then
        echo "parity_test: TOOL FAILURE cannot read $list"
        return 2
    fi
    if ! grep -q . "$list"; then
        echo "parity_test: TOOL FAILURE $list is empty"
        return 2
    fi
    if ! LC_ALL=C sort -c "$list" 2>/dev/null; then
        echo "parity_test: FAIL $list is not sorted under LC_ALL=C"
        rc=1
    fi
    for g in $(LC_ALL=C sort "$list" | LC_ALL=C uniq -d); do
        echo "parity_test: FAIL duplicate line: $g"
        rc=1
    done
    for g in $(LC_ALL=C comm -23 <(LC_ALL=C sort -u "$mk") <(LC_ALL=C sort -u "$list")); do
        echo "parity_test: FAIL gate only in the Makefile: $g (add a line to tools/parity-gates.txt)"
        rc=1
    done
    for g in $(LC_ALL=C comm -13 <(LC_ALL=C sort -u "$mk") <(LC_ALL=C sort -u "$list")); do
        echo "parity_test: FAIL gate only in tools/parity-gates.txt: $g (remove the line)"
        rc=1
    done
    return "$rc"
}

# --- self-test (runs before anything reads the Makefile or the real list) ---
# Proves check_list can report each failure it claims to. It scrubs PATH and
# reads no machine state: every input is a fixture built in a temp dir.
if [ "${1:-}" = "--self-test" ]; then
    PATH=/usr/bin:/bin
    export PATH
    st_dir="$(mktemp -d)" || exit 2
    trap 'rm -rf "$st_dir"' EXIT
    st_fail=0
    mk() { printf '%s' "$2" > "$st_dir/$1"; }
    # expect NAME WANT_RC WANT_TEXT MAKE_FILE LIST_FILE
    expect() {
        local name="$1" want_rc="$2" want_text="$3" out rc
        out="$(check_list "$st_dir/$4" "$st_dir/$5" 2>&1)"
        rc=$?
        if [ "$rc" -ne "$want_rc" ]; then
            echo "self-test: FAIL $name: exit $rc, want $want_rc"
            st_fail=$((st_fail + 1))
        elif [ -n "$want_text" ] && ! printf '%s\n' "$out" | grep -qF "$want_text"; then
            echo "self-test: FAIL $name: output lacks '$want_text'"
            st_fail=$((st_fail + 1))
        else
            echo "self-test: ok   $name"
        fi
    }
    mk make $'aaa-probe\nbbb-probe\nccc-probe\n'
    mk good $'aaa-probe\nbbb-probe\nccc-probe\n'
    mk missing $'aaa-probe\nccc-probe\n'
    mk extra $'aaa-probe\nbbb-probe\nccc-probe\nzzz-probe\n'
    mk dup $'aaa-probe\nbbb-probe\nbbb-probe\nccc-probe\n'
    mk unsorted $'aaa-probe\nccc-probe\nbbb-probe\n'
    mk empty ''
    expect "matching list passes" 0 "" make good
    expect "gate only in Makefile" 1 "only in the Makefile: bbb-probe" make missing
    expect "gate only in list" 1 "only in tools/parity-gates.txt: zzz-probe" make extra
    expect "duplicate line" 1 "duplicate line: bbb-probe" make dup
    expect "unsorted file" 1 "not sorted" make unsorted
    expect "empty list" 2 "empty" make empty
    if [ "$st_fail" -ne 0 ]; then
        echo "parity_test --self-test: FAIL $st_fail case(s)"
        exit 1
    fi
    echo "parity_test --self-test: PASS"
    exit 0
fi

gates="$(./tools/parity.sh --list-make-gates)"
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "parity_test: --list-make-gates exited $rc"
    exit 2
fi

fail=0

tmp_gates="$(mktemp)"
printf '%s\n' "$gates" > "$tmp_gates"
check_list "$tmp_gates" tools/parity-gates.txt
lrc=$?
rm -f "$tmp_gates"
if [ "$lrc" -eq 2 ]; then
    exit 2
fi
if [ "$lrc" -ne 0 ]; then
    fail=1
fi
count="$(printf '%s\n' "$gates" | grep -c .)"

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
    echo "parity_test: PASS $count gates match tools/parity-gates.txt, mapping, arithmetic and allowlist correct"
fi
exit "$fail"
