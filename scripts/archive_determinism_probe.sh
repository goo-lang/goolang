#!/bin/bash
# archive-determinism probe — building lib/libgoo_runtime.a twice from the same
# objects must give a byte-identical archive.
#
# WHAT WENT WRONG. The recipe pipes an MRI script to `ar -M`, which stores each
# member's real mtime, uid, gid and mode. Two builds seconds apart produced
# archives differing by 206 bytes, while all 102 members extracted
# byte-identical. Every compiled object was already deterministic; the wrapper
# around them was not.
#
# This probe re-archives the ALREADY-BUILT objects rather than rebuilding them,
# so it costs seconds and needs no compiler. It sets the objects' mtimes to two
# clearly distinct fixed timestamps between the two archive builds, which is
# the condition that made the real builds differ.
#
# WHY FIXED TIMESTAMPS, NOT A BARE `touch`. `ar`'s member-mtime field has
# 1-second resolution. `ar x` stamps each extracted object with the CURRENT
# time, and a bare `touch` right before the second build also stamps "now" —
# on a fast machine, extract, first build, touch, and second build all land in
# the SAME wall-clock second, so the mtime never actually changes and the
# probe passes even with the `-D` fix reverted. Explicit `touch -d` timestamps,
# one far in the past and one far in the future, remove the race entirely.
set -u

PROBE="archive-determinism-probe"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LIB="$ROOT/lib/libgoo_runtime.a"

if [ ! -f "$LIB" ]; then
    echo "$PROBE: FAIL (lib/libgoo_runtime.a is missing — run 'make runtime-lib' first)"
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/m"
( cd "$WORK/m" && ar x "$LIB" ) || { echo "$PROBE: FAIL (could not extract $LIB)"; exit 1; }

count=$(find "$WORK/m" -name '*.o' | wc -l)
if [ "$count" -eq 0 ]; then
    echo "$PROBE: FAIL (archive extracted zero members — the probe would pass vacuously)"
    exit 1
fi

# Rebuild the archive twice in the recipe's own shape, with each build's
# objects stamped to a distinct fixed timestamp first.
mri() {  # $1 = output archive
    rm -f "$1"
    { echo "create $1"
      for o in "$WORK"/m/*.o; do echo "addmod $o"; done
      echo "save"; echo "end"; } | ar -D -M
    ranlib -D "$1"
}

touch -d '@1000000000' "$WORK"/m/*.o
mri "$WORK/a1.a"
touch -d '@2000000000' "$WORK"/m/*.o
mri "$WORK/a2.a"

if cmp -s "$WORK/a1.a" "$WORK/a2.a"; then
    echo "$PROBE: PASS ($count members, archive is byte-identical across mtime changes)"
    exit 0
fi

echo "$PROBE: FAIL — two archives of the same $count members differ."
echo "  \`ar\` is recording member mtime/uid/gid. The recipe needs 'ar -D -M' and 'ranlib -D'."
exit 1
