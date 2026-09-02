#!/usr/bin/env bash
# May a map take ownership of a key that comes from a LOCAL?
#
# ============================================================================
# GREEN AND IN verify-core since 2026-08-01 (ADR 0005). It was known-red from
# PR #281, and the history is worth keeping because the red was closed by a
# route nobody predicted.
#
# THE SPIKE THAT KEPT IT RED: delete the map-key escape mark (escape_core.c,
# the escape_mark in mark_lvalue_subscripts) and measure. Result, on
# bench/daemon/daemon.goo:
#
#   counts[f] = 1                 -> f IS released
#   counts[f] = counts[f] + 1     -> f is NOT released      <- the daemon
#
# The measurement was correct. The CONCLUSION drawn from it -- that condition 6,
# the loop-carried store, was the second blocker -- was wrong on both halves:
#
#   * deleting the mark was never the way in. The mark is what makes the map
#     the key's SOLE owner, so removing it was removing the thing that made the
#     reclamation safe. The actual obstacle was release_plan_key_is_owned
#     refusing every AST_IDENTIFIER, in a different module entirely.
#   * condition 6 is not involved. GOO_ARC_DEBUG=1 says `f` reads
#     RELEASE_NO_ESCAPES -- condition 1 -- and `f` is still refused there today.
#     It does not need to be released: the MAP frees the key.
#
# WHAT ACTUALLY CLOSED IT was the reason SET, so that "escapes only as a
# subscript" could be told apart from "escapes, and also leaves the function".
# Measured: 262,205 -> 82,205 bytes on the daemon, 180,000 reclaimed, valgrind
# clean, output unchanged against release-off and against `go run`.
#
# THE LESSON, since this file already carries one: a spike that deletes a rule
# and measures no change has shown that THAT rule is not the obstacle. It has
# not identified the obstacle, and naming one from the same experiment is a
# guess wearing a measurement's clothes.
# ============================================================================
#
# PR #272 gave a map the key it is HANDED as a fresh temporary. It refuses an
# AST_IDENTIFIER, because an identifier means another name holds the buffer.
# bench/daemon/daemon.goo writes `counts[f] = counts[f] + 1` with a local, and
# that shape is 180,000 bytes per 2,000 client messages.
#
# The local is never released — `counts[f]` marks it escaping, so condition 1
# refuses it — and that is exactly what leaves the map free to be its single
# owner.
#
# THE ASSERTION IS SCOPED TO `counted`, the one function whose key is safe to
# take. The three refusal functions leak their own local on purpose: each is a
# shape no map may own, and counting them here would make the probe demand work
# this change must NOT do.
#
# The dangerous direction is a map that takes a buffer somebody else still uses.
# escapesByReturn hands its local back to the caller, twoMaps hands one buffer
# to two maps, and viewKey hands a window into a buffer it never allocated.
# Each reads the BYTES afterwards, so a wrong release is an invalid read or a
# double free rather than a silent pass.
#
# Usage: scripts/arc_map_key_local_probe.sh

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
SRC="examples/arc_map_key_local_probe.goo"
WORK="${ARC_MAPKEY_WORKDIR:-build}"
fail=0

[ -x "$COMPILER" ] || { echo "arc-map-key-local-probe: FAIL (no bin/goo)"; exit 1; }
[ -f "$SRC" ] || { echo "arc-map-key-local-probe: FAIL (no $SRC)"; exit 1; }
mkdir -p "$WORK"

if ! command -v valgrind >/dev/null 2>&1; then
    echo "valgrind not found — SKIPPED"
    exit 0
fi

echo "=== arc-map-key-local-probe: a map owning a key that comes from a local ==="

GOO_ARC_RELEASE=0 "$COMPILER" -o "$WORK/arc_mk_off" "$SRC" > "$WORK/arc_mk_off.cerr" 2>&1 \
    || { echo "  FAIL (compile, release off)"; cat "$WORK/arc_mk_off.cerr"; exit 1; }
"$COMPILER" -o "$WORK/arc_mk_on" "$SRC" > "$WORK/arc_mk_on.cerr" 2>&1 \
    || { echo "  FAIL (compile, release on)"; cat "$WORK/arc_mk_on.cerr"; exit 1; }

# Bytes and record count NOT RECLAIMED by records whose stack names `counted`.
#
# DEFINITELY *AND* INDIRECTLY LOST, and the second one is not padding. With
# GOO_ARC_RELEASE=0 the map itself leaks, so its keys are reachable from it and
# valgrind calls them INDIRECTLY lost. Releasing the map is what moves them to
# directly lost (#270). Counting only `definitely` made the control read as ZERO,
# and the probe then claimed it measured nothing.
#
# Anchored to the frame line valgrind prints for a call, so a match is a stack
# frame and never a mention elsewhere in the log.
counted_lost() {
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
    if re.search(r"^\s*by 0x[0-9A-Fa-f]+: counted \(", line):
        total += pending
        records += 1
        pending = None
    elif not line.strip():
        pending = None
print(f"{total} {records}")
PY
}

valgrind --leak-check=full --show-leak-kinds=all \
    "$WORK/arc_mk_off" > "$WORK/arc_mk_off.out" 2> "$WORK/arc_mk_off.vg"
read -r lost_off recs_off <<< "$(counted_lost "$WORK/arc_mk_off.vg")"

# The control keeps TWO records, and the second one is not the keys: with no
# release at all the map's own entry nodes leak too. Releasing the map reclaims
# those, which is why the ON build below is expected to hold the keys ALONE.
if [ "${recs_off:-0}" -lt 1 ] || [ "${lost_off:-0}" -eq 0 ]; then
    echo "  FAIL: with GOO_ARC_RELEASE=0 counted leaked nothing — the probe measures nothing"
    fail=1
else
    echo "  release OFF: $lost_off bytes in $recs_off records (the keys and the map's nodes)"
fi

valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=none \
    --error-exitcode=99 \
    "$WORK/arc_mk_on" > "$WORK/arc_mk_on.out" 2> "$WORK/arc_mk_on.vg"
rc=$?
read -r lost_on recs_on <<< "$(counted_lost "$WORK/arc_mk_on.vg")"

# THE REFUSALS. A map that took a buffer another owner still holds shows up
# here, because every refusal function reads its local's bytes afterwards.
if [ $rc -ne 0 ] || grep -qE "Invalid read|Invalid write|Invalid free|double free" "$WORK/arc_mk_on.vg"; then
    echo "  FAIL: A MAP TOOK A BUFFER SOMEBODY ELSE STILL HOLDS (rc=$rc)"
    grep -B2 -A8 -E "Invalid read|Invalid free|double free" "$WORK/arc_mk_on.vg" | head -30
    fail=1
else
    echo "  refusals:    valgrind clean — the returned, two-map and view keys were kept"
fi

if [ "${recs_on:-9}" -ne 0 ] || [ "${lost_on:-1}" -ne 0 ]; then
    echo "  FAIL: counted still loses $lost_on bytes in $recs_on records — the map is not taking the key"
    fail=1
else
    echo "  reclaimed:   $lost_off -> 0 bytes, the map owns every key it was handed"
fi

if ! diff -q "$WORK/arc_mk_off.out" "$WORK/arc_mk_on.out" > /dev/null 2>&1; then
    echo "  FAIL: output differs between release OFF and ON"
    diff "$WORK/arc_mk_off.out" "$WORK/arc_mk_on.out" | head -10
    fail=1
else
    echo "  output:      identical with release off and on"
fi

if [ $fail -ne 0 ]; then echo "arc-map-key-local-probe: FAIL"; exit 1; fi
echo "arc-map-key-local-probe: PASS"
