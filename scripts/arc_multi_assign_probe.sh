#!/usr/bin/env bash
# Does a MULTI-ASSIGN target free what its slot held?
#
# `a, b = x, y` wrote into two slots and freed neither previous value.
# codegen_arc_release_before_store had exactly ONE caller -- the single-assign
# arm in expression_codegen.c -- so the identical shape spelled with a comma
# leaked. Carried unchanged across three handoffs as "untested rather than
# proven absent"; this is the test.
#
# A LEAK, NEVER A USE-AFTER-FREE. Nothing was freed early, the old buffers were
# simply never freed. So the failure this probe guards against is silent by
# construction: no crash, no valgrind error, correct output. Only the block
# count moves.
#
# MEASURED, before and after:
#
#   release OFF (control)      45,043 bytes / 2,002 blocks
#   release ON, before the fix 44,999 bytes / 2,000 blocks   <- reclaims nothing
#   release ON, after          23,043 bytes / 1,002 blocks
#
# 1,000 blocks, which is 2 per iteration x 500 iterations of `reassigned`, and
# the accounting closes exactly: the 1,002 left are `destructured` (1,000, and
# see below) plus `swapped` (2).
#
# THE ASSERTION IS SCOPED TO `reassigned`, the one function whose targets reach
# a RELEASE_OK verdict. The other two leak on purpose:
#
#   swapped       refused, reasons=RETURN. It is here as a CORRECTNESS control,
#                 not a reclamation one. Pass 1 reads both rvalues before pass 2
#                 writes either, so a release put in pass 1 would free a buffer
#                 the other target is about to store. If `a, b = b, a` stops
#                 printing LEFT, the release went in the wrong place.
#   destructured  refused, RELEASE_NO_NOT_OWNED. Its value comes from a Goo
#                 callee, and condition 2 cannot prove a Goo callee returns
#                 fresh memory -- param_escape's return_escapes describes the
#                 returned VALUE, not its freshness. The destructure STORE SITE
#                 is fixed all the same, and is a no-op until that changes.
#                 Read the verdicts yourself with:
#                   GOO_ARC_DEBUG=1 ./bin/goo -o /dev/null \
#                       examples/arc_multi_assign_probe.goo
#
# Usage: scripts/arc_multi_assign_probe.sh

set -uo pipefail

# git first, because these probes are normally run from anywhere in a
# checkout. A Bazel sh_test starts with $PWD already AT the runfiles root, but
# $0 and ${BASH_SOURCE[0]} resolve to <runfiles>/_main/tests/probes/<name> --
# a symlink one directory too deep for a dirname-based fallback to find its
# way back to the root. The fallback is $PWD itself: measured, it IS the
# runfiles root there, and the repo root here.
# Resolve COMPILER BEFORE the cd below. A Bazel sh_test passes a path relative
# to the runfiles root, which is the working directory on entry; after `cd
# "$ROOT"` a relative path no longer resolves, and the probe reports "no
# bin/goo" while holding a perfectly good compiler path.
case "${COMPILER:-}" in
    "") ;;
    /*) ;;
    *) COMPILER="$PWD/$COMPILER" ;;
esac

ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || ROOT="$PWD"
[ -n "$ROOT" ] || ROOT="$PWD"
cd "$ROOT" || exit 2

# COMPILER is the contract a Bazel sh_test uses to point this probe at the
# compiler it built: bin/goo does not exist inside the sandbox. Unset, this
# behaves exactly as it did.
COMPILER="${COMPILER:-$ROOT/bin/goo}"
SRC="examples/arc_multi_assign_probe.goo"
WORK="${ARC_MULTIASSIGN_WORKDIR:-build}"
fail=0

[ -x "$COMPILER" ] || { echo "arc-multi-assign-probe: FAIL (no bin/goo)"; exit 1; }
[ -f "$SRC" ] || { echo "arc-multi-assign-probe: FAIL (no $SRC)"; exit 1; }
mkdir -p "$WORK"

if ! command -v valgrind >/dev/null 2>&1; then
    # Under make, tests.yml greps verify-core.log for the SKIPPED line and
    # fails the job, so the skip is loud there. A Bazel test log is read by
    # nobody: the harness sets GOO_PROBE_NO_SKIP, and the skip is the failure.
    if [ "${GOO_PROBE_NO_SKIP:-0}" = 1 ]; then
        echo "arc-multi-assign-probe: FAIL (valgrind not found, and GOO_PROBE_NO_SKIP forbids a skip)"
        exit 1
    fi
    echo "valgrind not found — SKIPPED"
    exit 0
fi

echo "=== arc-multi-assign-probe: a, b = x, y must free what the slots held ==="

GOO_ARC_RELEASE=0 "$COMPILER" -O2 -o "$WORK/arc_ma_off" "$SRC" > "$WORK/arc_ma_off.cerr" 2>&1 \
    || { echo "  FAIL (compile, release off)"; cat "$WORK/arc_ma_off.cerr"; exit 1; }
"$COMPILER" -O2 -o "$WORK/arc_ma_on" "$SRC" > "$WORK/arc_ma_on.cerr" 2>&1 \
    || { echo "  FAIL (compile, release on)"; cat "$WORK/arc_ma_on.cerr"; exit 1; }

# Blocks still held at exit.
#
# WHY THE TOTAL AND NOT A PER-FUNCTION SCOPE. The first version of this probe
# counted only records whose stack frame named `reassigned`, the way
# arc_map_key_local_probe.sh scopes to `counted`. At -O2 that reads ZERO for
# both builds: the function is INLINED into main and the frame name is gone.
# The probe's own zero-control guard caught it, which is the only reason this
# comment is not a bug report.
#
# The total is exact here in a way a scoped count would not be, because the
# fixture's accounting closes to the block:
#
#   2,503 control  =  1,000 reassigned + 1,000 destructured + 2 swapped
#                     + 501 selfAppend
#   1,002 after    =                     1,000 destructured + 2 swapped
#
# BLOCKS, NOT BYTES, is the primary assertion. A byte total moves with the
# output path -- main reads os.Args and the runtime copies argv[0] to the heap
# unfreed -- so it drifts by a few bytes for reasons that have nothing to do
# with the compiler. The block count does not.
lost_blocks() {
    sed -n 's/^==[0-9]*==\s*definitely lost: [0-9,]* bytes in \([0-9,]*\) blocks/\1/p' "$1" \
        | tr -d ','
}
lost_bytes() {
    sed -n 's/^==[0-9]*==\s*definitely lost: \([0-9,]*\) bytes in [0-9,]* blocks/\1/p' "$1" \
        | tr -d ','
}

# GENUINE memory errors only. `--leak-check=full` makes valgrind count every
# LEAK RECORD in its ERROR SUMMARY, so on this fixture -- whose whole subject is
# a leak -- "ERROR SUMMARY: 0 errors" is unreachable by construction and testing
# for it fails a correct build. The errors that matter here are the ones a
# release in the wrong place produces.
real_errors() {
    grep -cE "Invalid (read|write|free)|Mismatched free|uninitialised value|Double free" "$1" \
        || true
}

out_off=$("$WORK/arc_ma_off")
out_on=$("$WORK/arc_ma_on")

# CORRECTNESS FIRST, and it outranks every byte below. A release in the wrong
# place makes `swapped` print the wrong string or read freed memory, and no
# amount of reclamation excuses that.
if [ "$out_off" != "$out_on" ]; then
    echo "  FAIL: output differs between release OFF and ON"
    echo "    off: $(echo "$out_off" | tr '\n' ' ')"
    echo "    on:  $(echo "$out_on"  | tr '\n' ' ')"
    fail=1
else
    echo "  output identical off/on: $(echo "$out_on" | tr '\n' ' ')"
fi

valgrind --leak-check=full "$WORK/arc_ma_off" > /dev/null 2> "$WORK/arc_ma_off.vg"
valgrind --leak-check=full "$WORK/arc_ma_on"  > /dev/null 2> "$WORK/arc_ma_on.vg"

off_blocks=$(lost_blocks "$WORK/arc_ma_off.vg")
on_blocks=$(lost_blocks "$WORK/arc_ma_on.vg")
off_bytes=$(lost_bytes "$WORK/arc_ma_off.vg")
on_bytes=$(lost_bytes "$WORK/arc_ma_on.vg")

echo "  release OFF: ${off_bytes} bytes in ${off_blocks} blocks"
echo "  release ON:  ${on_bytes} bytes in ${on_blocks} blocks"

# EXPECTED_RECLAIM is pinned, not compared loosely, and the fixture is fixed
# source so it can be. `>` alone would pass on 1 block out of 1,000 and call a
# 99.9%-broken fix green. Change this number in the commit that changes
# examples/arc_multi_assign_probe.goo, and give the reason.
EXPECTED_RECLAIM=1501

if [ -z "$off_blocks" ] || [ -z "$on_blocks" ]; then
    echo "  FAIL: could not read a block count from the valgrind log"
    fail=1
else
    reclaimed=$(( off_blocks - on_blocks ))
    echo "  reclaimed: ${reclaimed} blocks (expected ${EXPECTED_RECLAIM})"

    # THE CONTROL MUST LEAK, or the probe proves nothing. A control that reads
    # zero means the shape stopped allocating, not that the fix works, and a
    # probe that cannot tell those apart is worse than no probe.
    if [ "$off_blocks" -eq 0 ]; then
        echo "  FAIL: control leaked nothing — the probe is broken, not the compiler"
        fail=1
    elif [ "$reclaimed" -ne "$EXPECTED_RECLAIM" ]; then
        echo "  FAIL: expected ${EXPECTED_RECLAIM} blocks reclaimed, got ${reclaimed}"
        fail=1
    fi
fi

# The fix FREES memory, so the direction that matters most is that it frees
# nothing twice and reads nothing freed. Leak records are excluded on purpose --
# see real_errors above.
for b in off on; do
    errs=$(real_errors "$WORK/arc_ma_${b}.vg")
    if [ "$errs" -ne 0 ]; then
        echo "  FAIL: ${errs} genuine memory error(s) with release ${b}"
        grep -E "Invalid (read|write|free)|Mismatched free|uninitialised value" \
            "$WORK/arc_ma_${b}.vg" | head -3
        fail=1
    fi
done
[ "$fail" -eq 0 ] && echo "  valgrind: no invalid reads, writes or frees in either build"

if [ "$fail" -ne 0 ]; then
    echo "arc-multi-assign-probe: FAIL"
    exit 1
fi
echo "arc-multi-assign-probe: PASS"
