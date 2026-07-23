#!/bin/bash
# Arena RSS capstone: prove `arena { }` reclaims memory. For each probe below,
# compile it TWICE — once as written (`arena { }`, freed) and once with the
# arena replaced by a plain block (`{ }`, allocations leak) — and assert the
# arena build's peak resident memory is substantially below the leaking build's.
# Two reclaim shapes are covered:
#   arena_loop_reclaim_probe   — arena INSIDE a loop, freed on fall-through each
#                                iteration (Task 6).
#   arena_return_reclaim_probe — a function that RETURNS out of an arena, called
#                                in a loop; the arena is freed on the early-exit
#                                return path (early-exit-free follow-up). Without
#                                that free, every call leaks its arena (~810MB
#                                measured vs ~14MB).
# Run from repo root after `make` + the runtime archive are built.

set -u

fail() { echo "FAIL: $1"; exit 1; }
skip() { echo "arena-rss-probe: SKIPPED ($1)"; exit 0; }

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
COMPILER="${COMPILER:-$ROOT/bin/goo}"
PROBES="arena_loop_reclaim_probe arena_return_reclaim_probe arena_loopexit_reclaim_probe"

[ -x "$COMPILER" ] || fail "compiler not found at $COMPILER (run 'make')"

# /usr/bin/time -v is what reports "Maximum resident set size"; the bash builtin
# `time` does not. Skip loudly if it (or its -v support) is unavailable rather
# than silently pass.
TIME_BIN="/usr/bin/time"
[ -x "$TIME_BIN" ] || skip "/usr/bin/time not found"
if ! "$TIME_BIN" -v true >/dev/null 2>&1; then
    skip "/usr/bin/time -v unsupported"
fi

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

# Peak RSS in KB via /usr/bin/time -v (line: "Maximum resident set size (kbytes): N").
peak_rss_kb() {
    "$TIME_BIN" -v "$1" >/dev/null 2>"$WORKDIR/time.txt" || return 1
    grep -F "Maximum resident set size" "$WORKDIR/time.txt" | grep -oE '[0-9]+' | tail -1
}

echo "=== arena-rss-probe: per-iteration/per-call reclamation (RSS) ==="
for name in $PROBES; do
    src="$ROOT/examples/$name.goo"
    [ -f "$src" ] || fail "missing $src"

    # No-arena variant: SAME program with `arena {` turned into a plain `{`, so
    # the identical allocations happen but are never reclaimed.
    noarena_src="$WORKDIR/$name.noarena.goo"
    sed 's/arena {/{/' "$src" > "$noarena_src"

    arena_exe="$WORKDIR/$name.arena"
    noarena_exe="$WORKDIR/$name.noarena"
    "$COMPILER" -o "$arena_exe"   "$src"          >"$WORKDIR/a.log" 2>&1 || { sed 's/^/    /' "$WORKDIR/a.log"; fail "$name: arena build failed"; }
    "$COMPILER" -o "$noarena_exe" "$noarena_src"  >"$WORKDIR/n.log" 2>&1 || { sed 's/^/    /' "$WORKDIR/n.log"; fail "$name: no-arena build failed"; }

    [ "$("$arena_exe")"   = "done" ] || fail "$name: arena build did not print 'done'"
    [ "$("$noarena_exe")" = "done" ] || fail "$name: no-arena build did not print 'done'"

    arena_kb="$(peak_rss_kb "$arena_exe")"     || fail "$name: could not measure arena RSS"
    noarena_kb="$(peak_rss_kb "$noarena_exe")" || fail "$name: could not measure no-arena RSS"
    [ -n "$arena_kb" ] && [ -n "$noarena_kb" ] && [ "$arena_kb" -gt 0 ] || skip "RSS unparseable in this environment"

    # The leaking build must be clearly larger. Require at least 1.5x headroom so
    # the gate is robust to RSS noise while still failing hard if reclamation
    # regresses (both builds would then leak and the ratio collapses toward 1.0).
    thresh_kb=$(( arena_kb * 3 / 2 ))
    if [ "$noarena_kb" -le "$thresh_kb" ]; then
        fail "$name: arena did not reclaim (arena=${arena_kb}KB no-arena=${noarena_kb}KB, expected no-arena > 1.5x arena)"
    fi
    echo "$name: PASS (arena=${arena_kb}KB vs no-arena=${noarena_kb}KB peak RSS)"
done

echo "arena-rss-probe: PASS (reclamation confirmed)"
