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

# Extra flags added to every bazel invocation below. CI needs it: ubuntu's
# llvm-dev ships only a VERSIONED llvm-config, so the coverage query -- which
# loads //... and therefore //src/codegen -- would be refused by the repository
# rule without --repo_env=GOO_LLVM_CONFIG.
#
# DO NOT PUT --repo_env=CC HERE. The configs below pin clang deliberately, and
# an override would make every assertion pass under gcc while checking nothing:
# gcc does not declare layering_check, so the defect would build, the "REFUSED"
# assertion would fail, and the honest failure would be blamed on the gate.
BAZEL_EXTRA=${BAZEL_EXTRA:-}

cd "$(dirname "${BASH_SOURCE[0]}")/.."
rc=0

build() {
    # shellcheck disable=SC2086
    "$BAZEL" build "$@" $BAZEL_EXTRA >/dev/null 2>&1
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

# THE RATCHET. //include:headers exported all 79 headers to every target, which
# made layering_check unable to refuse anything. It was deleted once its last
# consumer was migrated, and goo_cc_library has no opt-out any more.
#
# This asserts it has not come back. A reintroduced blob would turn every
# assertion above green while checking nothing, and nothing else in the tree
# would notice.
echo "no opt-out:"
# shellcheck disable=SC2086
if "$BAZEL" query '//include:headers' $BAZEL_EXTRA >/dev/null 2>&1; then
    printf '  WRONG //include:headers exists again -- a target can opt out of declaring headers\n'
    rc=1
else
    printf '  OK    //include:headers is gone, so no target can opt out\n'
fi

# The header libraries that replaced it. Printed rather than asserted: the
# right number is whatever the includes require, and it moves with the code.
# shellcheck disable=SC2086
hdrlibs=$("$BAZEL" query 'kind("cc_library", //include:*)' $BAZEL_EXTRA 2>/dev/null | grep -c '_h$')
printf '  %d header libraries in //include\n' "$hdrlibs"

[ $rc -eq 0 ] && echo "verify_layering: both gates have teeth"
exit $rc
