#!/usr/bin/env bash
# Run the golden suite against the BAZEL-built compiler and archive.
#
# scripts/run_golden.sh needs no change: it already takes COMPILER and EX_DIR
# from the environment, and the compiler takes GOO_RUNTIME and GOOROOT. This
# only supplies them.
#
# EXPECTED RESULT: the fixtures that run_golden.sh runs (every examples/*.goo
# with a sibling .expected.txt) split into a pass group and a fail group. The
# fail group is the committed list tools/golden_bazel_known_fail.txt, which
# names each fixture and its reason (all need NNG, phase 3c). The pass count
# is COMPUTED from the tree, so adding a fixture needs no edit here. The check
# compares the SET of failing names, not only counts, so it turns red when
#   - a pass-group fixture starts to fail (a regression),
#   - a listed fixture starts to pass (the list must stay exact),
#   - a listed name is no longer a fixture, or
#   - the suite runs zero fixtures.
# GOLDEN_BAZEL_REPLAY=<file> replays captured run_golden.sh output in place of
# the run, so the red paths can be proved without a 3-minute suite.
#
# Exit codes: 0 exactly the expected split, 1 anything else, 2 a tool failed.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

COMPILER_BIN="${COMPILER_BIN:-bazel-bin/src/compiler/goo}"
ARCHIVE="${ARCHIVE:-bazel-bin/src/runtime/libgoo_runtime.a}"
KNOWN_FAIL="${KNOWN_FAIL:-tools/golden_bazel_known_fail.txt}"
EX_DIR="${EX_DIR:-examples}"

[ -r "$KNOWN_FAIL" ] || { echo "golden_bazel: TOOL FAILURE $KNOWN_FAIL missing"; exit 2; }

# The same fixture rule as run_golden.sh: a .goo with a sibling .expected.txt.
total=0
for f in "$EX_DIR"/*.goo; do
    [ -f "${f%.goo}.expected.txt" ] && total=$((total + 1))
done
if [ "$total" -eq 0 ]; then
    echo "golden_bazel: FAIL no fixture found in '$EX_DIR' -- the suite would run nothing"
    exit 1
fi

known="$(grep -vE '^[[:space:]]*(#|$)' "$KNOWN_FAIL" | cut -f1 | LC_ALL=C sort)"
EXPECT_FAIL="${EXPECT_FAIL:-$(printf '%s\n' "$known" | grep -c .)}"
EXPECT_PASS="${EXPECT_PASS:-$((total - EXPECT_FAIL))}"

if [ -n "${GOLDEN_BAZEL_REPLAY:-}" ]; then
    out="$(cat "$GOLDEN_BAZEL_REPLAY")"
else
    [ -x "$COMPILER_BIN" ] || { echo "golden_bazel: TOOL FAILURE $COMPILER_BIN missing"; exit 2; }
    [ -r "$ARCHIVE" ]      || { echo "golden_bazel: TOOL FAILURE $ARCHIVE missing"; exit 2; }
    out="$(COMPILER="$COMPILER_BIN" GOO_RUNTIME="$root/$ARCHIVE" GOOROOT="$root" \
           bash scripts/run_golden.sh 2>&1)"
fi

summary="$(printf '%s\n' "$out" | grep -oE '[0-9]+ passed, [0-9]+ failed' | tail -1)"
if [ -z "$summary" ]; then
    echo "golden_bazel: TOOL FAILURE no summary line from run_golden.sh"
    printf '%s\n' "$out" | tail -20
    exit 2
fi

passed="$(printf '%s\n' "$summary" | awk '{print $1}')"
failed="$(printf '%s\n' "$summary" | awk '{print $3}')"
echo "golden_bazel: $summary (expected $EXPECT_PASS passed, $EXPECT_FAIL failed)"

actual="$(printf '%s\n' "$out" | grep -E '^FAIL  ' | awk '{print $2}' | LC_ALL=C sort)"
unexpected="$(comm -13 <(printf '%s\n' "$known") <(printf '%s\n' "$actual") | grep .)"
stale="$(comm -23 <(printf '%s\n' "$known") <(printf '%s\n' "$actual") | grep .)"

if [ "$passed" -eq "$EXPECT_PASS" ] && [ "$failed" -eq "$EXPECT_FAIL" ] \
   && [ -z "$unexpected" ] && [ -z "$stale" ]; then
    echo "golden_bazel: PASS the Bazel-built compiler and archive behave as expected"
    exit 0
fi

[ -n "$unexpected" ] && printf 'golden_bazel: REGRESSION fails but is not in %s: %s\n' "$KNOWN_FAIL" "$(echo $unexpected)"
[ -n "$stale" ]      && printf 'golden_bazel: STALE listed in %s but did not fail: %s\n' "$KNOWN_FAIL" "$(echo $stale)"
echo "golden_bazel: FAIL split moved -- fix the regression, or edit $KNOWN_FAIL"
exit 1
