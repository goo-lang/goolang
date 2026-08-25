#!/usr/bin/env bash
# Run the golden suite against the BAZEL-built compiler and archive.
#
# scripts/run_golden.sh needs no change: it already takes COMPILER and EX_DIR
# from the environment, and the compiler takes GOO_RUNTIME and GOOROOT. This
# only supplies them.
#
# EXPECTED RESULT THIS PHASE: 486 passed, 9 failed. The 9 are far_shim_probe
# and eight lanes_* probes, which reach the far transport and therefore need
# NNG -- phase 3c. The count is asserted EXACTLY rather than tolerated, so
# that a tenth failure is a regression and a fixed lanes probe is a visible
# change rather than a silently absorbed one.
#
# Exit codes: 0 exactly the expected split, 1 anything else, 2 a tool failed.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

COMPILER_BIN="${COMPILER_BIN:-bazel-bin/src/compiler/goo}"
ARCHIVE="${ARCHIVE:-bazel-bin/src/runtime/libgoo_runtime.a}"
EXPECT_PASS="${EXPECT_PASS:-486}"
EXPECT_FAIL="${EXPECT_FAIL:-9}"

[ -x "$COMPILER_BIN" ] || { echo "golden_bazel: TOOL FAILURE $COMPILER_BIN missing"; exit 2; }
[ -r "$ARCHIVE" ]      || { echo "golden_bazel: TOOL FAILURE $ARCHIVE missing"; exit 2; }

out="$(COMPILER="$COMPILER_BIN" GOO_RUNTIME="$root/$ARCHIVE" GOOROOT="$root" \
       bash scripts/run_golden.sh 2>&1)"

summary="$(printf '%s\n' "$out" | grep -oE '[0-9]+ passed, [0-9]+ failed' | tail -1)"
if [ -z "$summary" ]; then
    echo "golden_bazel: TOOL FAILURE no summary line from run_golden.sh"
    printf '%s\n' "$out" | tail -20
    exit 2
fi

passed="$(printf '%s\n' "$summary" | awk '{print $1}')"
failed="$(printf '%s\n' "$summary" | awk '{print $3}')"
echo "golden_bazel: $summary (expected $EXPECT_PASS passed, $EXPECT_FAIL failed)"

if [ "$passed" -eq "$EXPECT_PASS" ] && [ "$failed" -eq "$EXPECT_FAIL" ]; then
    echo "golden_bazel: PASS the Bazel-built compiler and archive behave as expected"
    exit 0
fi

printf '%s\n' "$out" | grep -E '^FAIL' | head -20
echo "golden_bazel: FAIL split moved -- if a lanes/far probe was fixed, update EXPECT_*"
exit 1
