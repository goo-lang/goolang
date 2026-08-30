#!/usr/bin/env bash
#
# Prove the two boundary gates can refuse something.
#
# Four assertions, and the third is the one that makes the second mean
# anything:
#
#   1. //testing/layering:ok             builds under --config=layering
#   2. //testing/layering:violation      FAILS under --config=layering
#   3. //testing/layering:violation      BUILDS with the feature off
#   4. //testing/layering:visibility_violation FAILS at analysis, any config
#
# Without 3, a defect that fails either way would prove only that the code is
# broken, not that the gate saw it.
#
# Exit codes follow the repo convention:
#   0  both gates have teeth
#   1  a gate failed: something did not behave as asserted
#   2  a tool failed: bazel could not run
#
# Status is read directly from bazel, never after a pipe: a pipeline reports
# only its last stage, which would hide a red result entirely.

set -uo pipefail

BAZEL=${BAZEL:-bazel}
cd "$(dirname "${BASH_SOURCE[0]}")/.."
rc=0

build() {
    "$BAZEL" build "$@" >/dev/null 2>&1
    return $?
}

expect() {
    local what=$1 want=$2 got=$3
    if [ "$want" = "pass" ] && [ "$got" -eq 0 ]; then
        printf '  OK    %s\n' "$what"; return 0
    fi
    if [ "$want" = "fail" ] && [ "$got" -ne 0 ]; then
        printf '  OK    %s\n' "$what"; return 0
    fi
    printf '  WRONG %s (exit %d, wanted %s)\n' "$what" "$got" "$want"
    return 1
}

echo "layering_check:"
build //testing/layering:ok --config=layering
expect "the control builds under --config=layering" pass $? || rc=1
build //testing/layering:violation --config=layering
expect "the defect is REFUSED under --config=layering" fail $? || rc=1
build //testing/layering:violation --config=clang
expect "the same defect BUILDS with the feature off" pass $? || rc=1

echo "visibility:"
build //testing/layering:visibility_violation
expect "reaching for a private target FAILS analysis" fail $? || rc=1

# How much of the tree the check actually covers. A number nobody prints is a
# number nobody notices going down.
#
# TWO numbers, because the first one alone would flatter. A target that still
# takes //include:headers gets all 79 headers and cannot violate layering, so
# the exempt count is the honest measure of what is left to do.
covered=$(grep -rc 'strict_hdrs = True' src/*/BUILD 2>/dev/null | awk -F: '{s+=$2} END {print s+0}')
total=$(grep -rc '^goo_cc_library(' src/*/BUILD 2>/dev/null | awk -F: '{s+=$2} END {print s+0}')
exempt=$("$BAZEL" query 'rdeps(//..., //include:headers, 1)' 2>/dev/null \
         | grep -cv '^//include:headers$')
echo "coverage:"
printf '  %d of %d goo_cc_library targets in src/ set strict_hdrs = True\n' "$covered" "$total"
printf '  %d targets still take //include:headers and are EXEMPT\n' "$exempt"
if [ "$covered" -eq 0 ]; then
    echo "  WRONG no target is covered, so the gate is vacuous"
    rc=1
fi

[ $rc -eq 0 ] && echo "verify_layering: both gates have teeth"
exit $rc
