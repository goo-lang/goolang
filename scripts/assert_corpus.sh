#!/bin/bash
# assert-corpus: run the whole fixture corpus through an ASSERT-ENABLED compiler.
#
# WHY THIS EXISTS. `make verify-core` builds the production compiler, where
# GOO_ASSERT compiles to (void)0. So every gate in verify-core can be green
# while every assert in the tree is dead code. An assert that no build ever
# evaluates is not a check, it is a comment with parentheses.
#
# This is the target that makes them earn their keep: build with GOO_DEBUG and
# push all ~751 fixtures through it, so each assert is evaluated on real input
# rather than on a synthetic probe.
#
# NOT IN verify-core, and the reason is mechanical rather than a judgement:
# `make debug` and `make lexer` both write bin/goo. Running this inside
# verify-core would swap the production binary out from under the other 197
# gates mid-run. It rebuilds bin/goo twice and takes several minutes, so it
# belongs in a pre-release sweep, not in the per-change gate.
#
# scripts/goo_assert_probe.sh is the fast companion and IS in verify-core: it
# checks that the three build modes behave, which is the property that would
# otherwise rot silently.
#
# LEAVES A DEBUG BINARY IN PLACE. Run `make lexer` afterwards, or let the next
# verify-core rebuild it. The script says so at the end rather than doing it
# silently, because a caller may want to keep the debug build to reproduce a
# failure.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

TIMEOUT="${FIXTURE_TIMEOUT:-30}"
MAX_REPORT="${MAX_REPORT:-10}"

echo "=== assert-corpus: the fixture corpus against a GOO_DEBUG compiler ==="

echo "--- building with -DGOO_DEBUG (asserts ON, ~4x slower) ---"
make clean >/dev/null 2>&1
if ! make debug >"$WORK/build.log" 2>&1; then
    echo "assert-corpus: FAIL — the debug build did not compile"
    grep -E "error:" "$WORK/build.log" | head -10
    exit 1
fi

# Instrument check. If GOO_DEBUG did not actually reach the compiler's objects,
# every assert below is a no-op and this script would report a clean sweep
# while checking nothing.
#
# Ask the BINARY, do not guess from its bytes. Two earlier versions of this
# check were both wrong, and each would have reported confidently:
#
#   1. grep for "ASSERT FAILED". Never contiguous — goo_assert_fail prints
#      "goo: %s FAILED" with the kind as a separate argument. Reported BROKEN
#      against a debug build whose asserts demonstrably fired.
#   2. grep for the format string "goo: %s FAILED". Present in ALL THREE builds
#      (61 copies), because goo_assert_fail is an unreferenced static that gcc
#      keeps at -O0, and its literal sits in .rodata either way. Could not
#      distinguish anything.
#
# `goo --version` reports the build directly, so there is nothing to infer.
if ! ./bin/goo --version 2>/dev/null | grep -q "^asserts: on"; then
    echo "assert-corpus: BROKEN — bin/goo reports:"
    ./bin/goo --version 2>&1 | sed 's/^/      /'
    echo "  GOO_DEBUG did not reach the build, so every assert is compiled out."
    echo "  A clean sweep here would mean nothing."
    exit 1
fi
echo "  bin/goo reports: $(./bin/goo --version | grep '^asserts:')"

mapfile -t FIXTURES < <(ls examples/*.goo tests/golden/reject/*.goo 2>/dev/null)
[ "${#FIXTURES[@]}" -gt 0 ] || { echo "assert-corpus: BROKEN — empty corpus"; exit 1; }

echo "--- compiling ${#FIXTURES[@]} fixtures ---"
aborted=0
ran=0
for f in "${FIXTURES[@]}"; do
    timeout "$TIMEOUT" ./bin/goo -o "$WORK/out" "$f" >/dev/null 2>"$WORK/err"
    rc=$?
    ran=$((ran + 1))
    # 128+SIGABRT(6)=134. A reject fixture exits 1 by design, and that is not a
    # failure here: this script asks whether an INVARIANT broke, not whether the
    # program compiled.
    if [ "$rc" -ge 134 ] && [ "$rc" -le 139 ]; then
        aborted=$((aborted + 1))
        if [ "$aborted" -le "$MAX_REPORT" ]; then
            echo "  ABORT rc=$rc  $f"
            sed 's/^/      /' "$WORK/err" | head -3
        fi
    fi
done

echo
echo "  fixtures compiled: $ran"
echo "  assert aborts:     $aborted"
[ "$aborted" -gt "$MAX_REPORT" ] && echo "  (only the first $MAX_REPORT are shown)"
echo
echo "  NOTE: bin/goo is now a DEBUG build. Run 'make lexer' to restore it."

if [ "$aborted" -eq 0 ]; then
    echo "assert-corpus: PASS ($ran fixtures, no invariant broken)"
    exit 0
fi
echo "assert-corpus: FAIL ($aborted fixture(s) broke an invariant)"
exit 1
