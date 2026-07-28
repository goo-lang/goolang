#!/bin/bash
# alloc-doors probe: Goo-visible heap memory must come from ONE door.
#
# Why this exists. The ARC header-feasibility audit (docs/superpowers/specs/
# 2026-07-28-arc-header-feasibility-audit.md) found a live asymmetry: a slice's
# backing store was allocated by a RAW calloc in goo_slice_alloc but grown by
# goo_realloc. That works today only because both are bare libc calls. Under
# ANY scheme that puts a header in front of the payload — a refcount for ARC, or
# a region tag — goo_realloc would offset a pointer that never had a header, and
# corrupt the heap.
#
# So the invariant is structural, and it is worth asserting before the header
# exists rather than debugging it afterwards: inside runtime.c, only goo_alloc,
# goo_realloc and goo_free may name a libc allocator.
#
# SCOPE. runtime.c only. Deliberately NOT the whole runtime:
#   - arena.c allocates its own BLOCKS with malloc. Those are the region's
#     backing store, never a Goo object, and they must stay raw.
#   - platform.c and io.c own private buffers that never become Goo values.
#   - concurrency.c/channels.c allocate scheduler and channel internals.
# Those are listed in the audit as known exemptions. If Goo-visible allocation
# ever moves into one of them, add it here and to the audit together.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/src/runtime/runtime.c"

fail() { echo "alloc-doors-probe: FAIL — $1"; exit 1; }

[ -f "$SRC" ] || fail "missing $SRC"

echo "=== alloc-doors-probe: one allocation door for Goo-visible memory ==="

# Every line in runtime.c that names a libc allocator, with its function.
# awk tracks the enclosing top-level function by its opening line.
# Comment lines are skipped: several doc comments legitimately NAME calloc
# while explaining why the code no longer calls it.
offenders="$(awk '
  /^[[:space:]]*(\/\/|\*|\/\*)/ { next }
  /^[A-Za-z_].*\)[[:space:]]*\{[[:space:]]*$/ { fn=$0; sub(/\(.*/,"",fn); sub(/.*[[:space:]\*]/,"",fn) }
  /(^|[^_[:alnum:]])(malloc|calloc|realloc|free)[[:space:]]*\(/ {
    if (fn != "goo_alloc" && fn != "goo_realloc" && fn != "goo_free")
      printf "  %s:%d  in %s()  ->  %s\n", "runtime.c", NR, fn, $0
  }
' "$SRC")"

if [ -n "$offenders" ]; then
    echo "$offenders"
    fail "a libc allocator is named outside goo_alloc/goo_realloc/goo_free"
fi

echo "  runtime.c: only goo_alloc, goo_realloc and goo_free name a libc allocator"
echo "alloc-doors-probe: PASS"
