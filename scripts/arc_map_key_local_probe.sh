#!/usr/bin/env bash
# May a map take ownership of a key that comes from a LOCAL?
#
# ============================================================================
# THIS PROBE IS KNOWN-RED, AND IT IS DELIBERATELY IN NO GATE. Do not add it to
# VERIFY_ALL_DEPS to "fix" the red — the red is the point, and it records work
# nobody has done. Run it by hand with `make arc-map-key-local-probe`.
#
# It was written for a change that a SPIKE then invalidated, and the spike's
# result is why the probe is kept rather than deleted.
#
# THE SPIKE: delete the map-key escape mark (escape_core.c, the escape_mark in
# mark_lvalue_subscripts) and measure. Result, on bench/daemon/daemon.goo:
#
#   counts[f] = 1                 -> f IS released
#   counts[f] = counts[f] + 1     -> f is NOT released      <- the daemon
#
# So the map-key mark is NOT what refuses the daemon's `f`. A SECOND rule does:
# condition 6, the loop-carried store. `counts[g] = counts[g] + 1` mentions `g`
# in the RHS of a store whose target outlives the iteration, and mark_mentions
# marks it — by design, since that rule is a MENTION and not a FLOW.
#
# CONSEQUENCE: taking the key is necessary and NOT sufficient. Removing the
# escape mark on its own reclaims ZERO bytes from the daemon. Both .handoff.md
# and the plan that produced this probe said otherwise, and both were wrong.
# Closing this needs BOTH a reason bit in escape_core and precision in
# condition 6 — two modules that have each produced a use-after-free.
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

ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "FATAL: not in a git repository" >&2; exit 2; }
cd "$ROOT" || exit 2

COMPILER="./bin/goo"
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
