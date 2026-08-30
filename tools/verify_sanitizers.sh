#!/usr/bin/env bash
#
# Prove that each sanitizer config can report a failure.
#
# For each deliberate defect in //testing/teeth, assert TWO things:
#   1. It FAILS when built under its own sanitizer config.
#   2. It PASSES when built without that config.
#
# Assertion 2 matters as much as assertion 1. A defect that fails in both cases
# proves only that the code is broken; it says nothing about the sanitizer. The
# defects are chosen so that an uninstrumented build genuinely exits 0 -- an
# overflow into malloc padding, a signed wrap, a race that only loses updates.
#
# Exit codes follow the repo convention:
#   0  every config has teeth
#   1  a gate failed: a config did not behave as asserted
#   2  a tool failed: bazel could not build or run something
#
# `bazel test` exit codes: 0 = all tests passed, 3 = built but a test failed.
# Every other non-zero status is a TOOL failure, never a test verdict. Status is
# read directly from bazel and never after a pipe, because a pipeline reports
# only its last stage and would hide a red result.

set -uo pipefail

BAZEL=${BAZEL:-bazel}
# Extra flags for every invocation. CI passes the llvm-config path here.
# DO NOT put --repo_env=CC here: the sanitizer configs pin clang on purpose,
# and gcc on this machine cannot link -fsanitize=address at all.
BAZEL_EXTRA=${BAZEL_EXTRA:-}

cd "$(dirname "${BASH_SOURCE[0]}")/.."
rc=0

bazel_status() {
    # shellcheck disable=SC2086
    "$BAZEL" test "$@" $BAZEL_EXTRA --test_output=summary >/dev/null 2>&1
    return $?
}

check_config() {
    local name=$1 target=$2 with without

    bazel_status "$target" "--config=$name"; with=$?
    bazel_status "$target"; without=$?

    if [ "$with" -ne 0 ] && [ "$with" -ne 3 ]; then
        printf '  TOOL FAILURE  %-6s bazel exit %d under --config=%s\n' "$name" "$with" "$name"
        return 2
    fi
    if [ "$without" -ne 0 ] && [ "$without" -ne 3 ]; then
        printf '  TOOL FAILURE  %-6s bazel exit %d with no config\n' "$name" "$without"
        return 2
    fi
    if [ "$with" -eq 3 ] && [ "$without" -eq 0 ]; then
        printf '  HAS TEETH     %-6s red under --config=%s, green without it\n' "$name" "$name"
        return 0
    fi
    if [ "$with" -eq 0 ]; then
        printf '  NO TEETH      %-6s the defect PASSED under --config=%s\n' "$name" "$name"
    else
        printf '  INVALID       %-6s the defect fails with the config OFF too\n' "$name"
    fi
    return 1
}

echo "sanitizer teeth:"
for pair in "asan //testing/teeth:asan_defect" \
            "ubsan //testing/teeth:ubsan_defect" \
            "tsan //testing/teeth:tsan_defect"; do
    # shellcheck disable=SC2086
    set -- $pair
    check_config "$1" "$2"
    r=$?
    if [ "$r" -eq 2 ]; then rc=2; elif [ "$r" -eq 1 ] && [ "$rc" -ne 2 ]; then rc=1; fi
done

# What the sanitizers actually cover. src/runtime opts out for the linker
# reason recorded in src/runtime/BUILD, so a green tsan run says nothing about
# the goroutine scheduler. Printing the count keeps that visible.
exempt=$(grep -c 'copts = NO_SANITIZE_COPTS' src/runtime/BUILD 2>/dev/null || echo 0)
echo "coverage:"
printf '  %d src/runtime targets opt OUT of instrumentation (linker constraint)\n' "$exempt"
if [ "$exempt" -eq 0 ]; then
    echo "  WRONG src/runtime is instrumented; the probe links will fail"
    rc=1
fi

[ $rc -eq 0 ] && echo "verify_sanitizers: every config has teeth"
exit $rc
