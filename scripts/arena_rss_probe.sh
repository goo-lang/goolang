#!/bin/bash
# Arena RSS capstone (Task 9): prove `arena { }` reclaims memory. Compile the
# same 100k-iteration temporary-building loop TWICE — once with the `arena { }`
# block (freed each iteration) and once with the arena replaced by a plain block
# (allocations leak, the allocate-and-leak baseline) — and assert the arena
# build's peak resident memory is substantially below the leaking build's. This
# is the concrete "parts are now Zig-like" proof from the arena plan. Run from
# repo root after `make` + the runtime archive are built.

set -u

fail() { echo "FAIL: $1"; exit 1; }
skip() { echo "arena-rss-probe: SKIPPED ($1)"; exit 0; }

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
COMPILER="${COMPILER:-$ROOT/bin/goo}"
ARENA_SRC="$ROOT/examples/arena_loop_reclaim_probe.goo"

[ -x "$COMPILER" ] || fail "compiler not found at $COMPILER (run 'make')"
[ -f "$ARENA_SRC" ] || fail "missing $ARENA_SRC"

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

# No-arena variant: the SAME program with `arena {` turned into a plain `{`, so
# the identical allocations happen but are never reclaimed.
NOARENA_SRC="$WORKDIR/noarena.goo"
sed 's/arena {/{/' "$ARENA_SRC" > "$NOARENA_SRC"

ARENA_EXE="$WORKDIR/arena"
NOARENA_EXE="$WORKDIR/noarena"
"$COMPILER" -o "$ARENA_EXE"   "$ARENA_SRC"   >"$WORKDIR/a.log" 2>&1 || { sed 's/^/    /' "$WORKDIR/a.log"; fail "arena build failed"; }
"$COMPILER" -o "$NOARENA_EXE" "$NOARENA_SRC" >"$WORKDIR/n.log" 2>&1 || { sed 's/^/    /' "$WORKDIR/n.log"; fail "no-arena build failed"; }

# Peak RSS in KB via /usr/bin/time -v (line: "Maximum resident set size (kbytes): N").
peak_rss_kb() {
    "$TIME_BIN" -v "$1" >/dev/null 2>"$WORKDIR/time.txt" || return 1
    grep -F "Maximum resident set size" "$WORKDIR/time.txt" | grep -oE '[0-9]+' | tail -1
}

# Both must actually run to completion (print "done").
[ "$("$ARENA_EXE")"   = "done" ] || fail "arena build did not print 'done'"
[ "$("$NOARENA_EXE")" = "done" ] || fail "no-arena build did not print 'done'"

ARENA_KB="$(peak_rss_kb "$ARENA_EXE")"   || fail "could not measure arena RSS"
NOARENA_KB="$(peak_rss_kb "$NOARENA_EXE")" || fail "could not measure no-arena RSS"
[ -n "$ARENA_KB" ] && [ -n "$NOARENA_KB" ] && [ "$ARENA_KB" -gt 0 ] || skip "RSS unparseable in this environment"

# The leaking build must be clearly larger. Measured ~8100 vs ~1950 KB (~4x);
# require at least 1.5x headroom so the gate is robust to RSS noise while still
# failing hard if arena reclamation regresses (both builds would then leak and
# the ratio would collapse toward 1.0).
THRESH_KB=$(( ARENA_KB * 3 / 2 ))
if [ "$NOARENA_KB" -le "$THRESH_KB" ]; then
    fail "arena did not reclaim: arena=${ARENA_KB}KB no-arena=${NOARENA_KB}KB (expected no-arena > 1.5x arena)"
fi

echo "arena-rss-probe: PASS (arena=${ARENA_KB}KB vs no-arena=${NOARENA_KB}KB peak RSS — arena reclaims per-iteration)"
