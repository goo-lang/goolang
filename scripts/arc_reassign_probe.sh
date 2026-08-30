#!/usr/bin/env bash
# Does a reassigned local release the value it is dropping?
#
# ============================================================================
# THIS PROBE IS GREEN AND IN verify-core AS OF THE REBOUND RELEASE. It landed
# KNOWN-RED with a five-step plan in its header; all five steps are done and the
# plan text is gone with them. Measured on bench/daemon/daemon.goo: 342,000 ->
# 262,000 bytes per 2,000 requests, the 80,000 this probe was written for.
#
# SELF-APPEND IS A REFUSAL HERE BECAUSE IT WAS MISSED. Every assertion below
# passed while the daemon printed NOTHING and reported 7 invalid reads:
# `parts = append(parts, x)` is not a rebind, and goo_slice_append's realloc
# frees the old base itself, so a release at that store reads a freed pointer.
# `selfAppend` was added for it, and the guard was mutation-tested -- disabling
# codegen's is_self_append check makes this probe FAIL on differing output.
# ============================================================================
#
# `last = handle(req)` in a loop replaces the string in the slot on every
# iteration, and nothing frees the one it replaced. Measured on
# bench/daemon/daemon.goo: 80,000 bytes per 2,000 client messages.
#
# Condition 4 refuses a rebound local outright today (`binding_count > 1` ->
# RELEASE_NO_REBOUND), because a second binding leaves the first value with no
# release site. Releasing AT THE STORE is that missing site: the assignment frees
# what the slot held, and scope exit frees whatever it holds last.
#
# NOT WHAT #276 DID. That work made a reassigned package-level GLOBAL immortal
# (3b4757a, 0f25db9, 9068675) so nothing frees it — the opposite direction.
#
# THE ASSERTION IS SCOPED TO `reassigned`. The three refusal functions leak on
# purpose: each carries a shape that must NOT release, and counting them here
# would make the probe demand work this change must not do.
#
# THE DANGEROUS DIRECTION IS AN ALIAS, AND IT IS NOT AN ESCAPE. `p := last`
# holds a second pointer without making the value outlive the function, so
# condition 1 never sees it. `aliased` reads p's BYTES after later stores, so a
# release that should not have fired is an invalid read here, not a silent pass.
#
# Usage: scripts/arc_reassign_probe.sh

set -uo pipefail

# git first, because these probes are normally run from anywhere in a
# checkout. A Bazel sandbox has no .git, so fall back to the script's own
# location -- which is the runfiles root there, and the repo root here. The
# probe must not depend on being inside a repository to know where it is.
# Resolve COMPILER BEFORE the cd below. A Bazel sh_test passes a path relative
# to the runfiles root, which is the working directory on entry; after `cd
# "$ROOT"` a relative path no longer resolves, and the probe reports "no
# bin/goo" while holding a perfectly good compiler path.
case "${COMPILER:-}" in
    "") ;;
    /*) ;;
    *) COMPILER="$PWD/$COMPILER" ;;
esac

ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" \
    || ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -n "$ROOT" ] || ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 2

# COMPILER is the contract a Bazel sh_test uses to point this probe at the
# compiler it built: bin/goo does not exist inside the sandbox. Unset, this
# behaves exactly as it did.
COMPILER="${COMPILER:-$ROOT/bin/goo}"
SRC="examples/arc_reassign_probe.goo"
WORK="${ARC_REASSIGN_WORKDIR:-build}"
fail=0

[ -x "$COMPILER" ] || { echo "arc-reassign-probe: FAIL (no bin/goo)"; exit 1; }
[ -f "$SRC" ] || { echo "arc-reassign-probe: FAIL (no $SRC)"; exit 1; }
mkdir -p "$WORK"

if ! command -v valgrind >/dev/null 2>&1; then
    echo "valgrind not found — SKIPPED"
    exit 0
fi

echo "=== arc-reassign-probe: releasing the value a reassigned local drops ==="

GOO_ARC_RELEASE=0 "$COMPILER" -o "$WORK/arc_ra_off" "$SRC" > "$WORK/arc_ra_off.cerr" 2>&1 \
    || { echo "  FAIL (compile, release off)"; cat "$WORK/arc_ra_off.cerr"; exit 1; }
"$COMPILER" -o "$WORK/arc_ra_on" "$SRC" > "$WORK/arc_ra_on.cerr" 2>&1 \
    || { echo "  FAIL (compile, release on)"; cat "$WORK/arc_ra_on.cerr"; exit 1; }

# Bytes and record count NOT RECLAIMED by records whose stack names `reassigned`.
#
# DEFINITELY *AND* INDIRECTLY LOST. The map-key probe learned this the hard way:
# a control that counts only `definitely` can read as ZERO when the thing holding
# the leaked memory leaks too, and a control that reads zero makes every
# measurement under it meaningless.
#
# Anchored to the frame line valgrind prints for a call, so a match is a stack
# frame and never a mention elsewhere in the log.
reassigned_lost() {
    python3 - "$1" <<'PY'
import re, sys
text = open(sys.argv[1], errors="replace").read()
total = 0
records = 0
pending = None
for line in text.splitlines():
    line = re.sub(r"^==\d+==\s?", "", line)
    m = re.match(r"\s*([\d,]+) bytes in [\d,]+ blocks are (?:definitely|indirectly) lost", line)
    if m:
        pending = int(m.group(1).replace(",", ""))
        continue
    if pending is None:
        continue
    if re.search(r"^\s*by 0x[0-9A-Fa-f]+: reassigned \(", line):
        total += pending
        records += 1
        pending = None
    elif not line.strip():
        pending = None
print(f"{total} {records}")
PY
}

valgrind --leak-check=full --show-leak-kinds=all \
    "$WORK/arc_ra_off" > "$WORK/arc_ra_off.out" 2> "$WORK/arc_ra_off.vg"
read -r lost_off recs_off <<< "$(reassigned_lost "$WORK/arc_ra_off.vg")"

if [ "${recs_off:-0}" -lt 1 ] || [ "${lost_off:-0}" -eq 0 ]; then
    echo "  FAIL: with GOO_ARC_RELEASE=0 reassigned leaked nothing — the probe measures nothing"
    fail=1
else
    echo "  release OFF: $lost_off bytes in $recs_off records (every value the loop dropped)"
fi

valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=none \
    --error-exitcode=99 \
    "$WORK/arc_ra_on" > "$WORK/arc_ra_on.out" 2> "$WORK/arc_ra_on.vg"
rc=$?
read -r lost_on recs_on <<< "$(reassigned_lost "$WORK/arc_ra_on.vg")"

# THE REFUSALS. An alias, a borrowed value and a local that leaves the function
# each read their bytes after the stores, so a release that should not have
# fired shows up as an invalid read or a double free.
if [ $rc -ne 0 ] || grep -qE "Invalid read|Invalid write|Invalid free|double free" "$WORK/arc_ra_on.vg"; then
    echo "  FAIL: A VALUE SOMEBODY ELSE STILL HOLDS WAS RELEASED (rc=$rc)"
    grep -B2 -A8 -E "Invalid read|Invalid free|double free" "$WORK/arc_ra_on.vg" | head -30
    fail=1
else
    echo "  refusals:    valgrind clean — the alias, the borrow and the escape were kept"
fi

if [ "${recs_on:-9}" -ne 0 ] || [ "${lost_on:-1}" -ne 0 ]; then
    echo "  FAIL: reassigned still loses $lost_on bytes in $recs_on records — the store is not releasing"
    fail=1
else
    echo "  reclaimed:   $lost_off -> 0 bytes, every dropped value released at its store"
fi

if ! diff -q "$WORK/arc_ra_off.out" "$WORK/arc_ra_on.out" > /dev/null 2>&1; then
    echo "  FAIL: output differs between release OFF and ON"
    diff "$WORK/arc_ra_off.out" "$WORK/arc_ra_on.out" | head -10
    fail=1
else
    echo "  output:      identical with release off and on"
fi

if [ $fail -ne 0 ]; then echo "arc-reassign-probe: FAIL"; exit 1; fi
echo "arc-reassign-probe: PASS"
