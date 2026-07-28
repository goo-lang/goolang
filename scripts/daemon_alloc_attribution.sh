#!/usr/bin/env bash
# Attribute bench/daemon's per-request retention to its allocation sites.
#
# ADR 0002 measured that the daemon shape retains ~1.63 KB per request forever.
# It never said WHERE. .handoff.md then rejected two release shortcuts on the
# strength of estimates derived by reading the source, and corrected itself once
# on exactly this point ("WRONG about its own payoff"). This script replaces the
# estimate with a measurement, so the scope of the first ARC release consumer is
# chosen from evidence.
#
# TWO INSTRUMENTS, because they answer different questions and disagree:
#
#   massif -> LIVE bytes at the peak snapshot. This is the retention, and it is
#             the number that matters. `parts` doubles 1->2->4->8, so realloc
#             frees three of the four buffers; only the last one is retained.
#   dhat   -> BLOCK COUNTS and total-allocated. This is the retain/release
#             traffic ARC would emit, and it is invisible to massif.
#
# Reading dhat's total-allocated as "the leak" overstates goo_slice_append by
# 2.1x. Reading massif alone hides that the program makes 47 allocations per
# request whose average payload is about 15 bytes. Report both.
#
# Usage: scripts/daemon_alloc_attribution.sh [massif_requests] [dhat_requests]
# Defaults: 50000 and 5000. dhat runs a smaller workload because it is slower,
# and retention is exactly linear in the request count (ADR 0002 measurement 1),
# so the per-request figures are comparable across the two counts.

set -euo pipefail

REQ_MASSIF="${1:-50000}"
REQ_DHAT="${2:-5000}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPILER="$ROOT/bin/goo"
SRC="$ROOT/bench/daemon/daemon.goo"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fail() { echo "daemon-alloc-attribution: FAIL ($*)" >&2; exit 1; }

[ -x "$COMPILER" ] || fail "no bin/goo — run make lexer first"
[ -f "$SRC" ] || fail "no $SRC"
command -v valgrind >/dev/null || fail "valgrind is not installed"

echo "=== daemon-alloc-attribution: building at -O2 ==="
"$COMPILER" -O2 -o "$WORK/daemon" "$SRC" >/dev/null 2>&1 || fail "goo build"

# Baseline peak RSS, so the massif total can be reconciled against the figure
# ADR 0002 and .handoff.md quote.
RSS_KB=$(/usr/bin/time -f '%M' "$WORK/daemon" "$REQ_MASSIF" 2>&1 >/dev/null | tail -1)
echo "peak RSS at $REQ_MASSIF requests: ${RSS_KB} KB"

echo
echo "=== massif: LIVE bytes for each site at the peak snapshot ==="
valgrind --tool=massif --massif-out-file="$WORK/massif.out" \
         --detailed-freq=1 --threshold=0.05 --max-snapshots=60 \
         "$WORK/daemon" "$REQ_MASSIF" >/dev/null 2>&1 || fail "massif run"

python3 - "$WORK/massif.out" "$REQ_MASSIF" <<'PY'
import re, sys
path, n = sys.argv[1], int(sys.argv[2])
text = open(path).read()

# Snapshots are separated by "#-----------"; the peak is the one whose
# heap_tree is "peak". Its tree lines carry "n0x... : bytes 0xaddr: fn (file:line)".
peak = None
for block in text.split("#-----------"):
    if "heap_tree=peak" in block:
        peak = block
if peak is None:
    sys.exit("massif produced no peak snapshot")

# Each tree line: " nK: BYTES 0xADDR: FUNC (FILE:LINE)". Keep leaf-most frames
# by attributing to the DEEPEST runtime.c frame on each path, which is the
# allocating helper rather than goo_alloc itself.
rows = []
for line in peak.splitlines():
    m = re.match(r"\s*n(\d+): (\d+) (.*)", line)
    if not m:
        continue
    nkids, nbytes, what = int(m.group(1)), int(m.group(2)), m.group(3)
    rows.append((len(line) - len(line.lstrip()), nkids, nbytes, what))

# A node's own retained bytes are its bytes minus its children's. massif's tree
# is already inclusive, so a leaf under goo_alloc is the real site.
leaves = []
for i, (indent, nkids, nbytes, what) in enumerate(rows):
    if nkids == 0 and nbytes > 0:
        # Walk back up for the nearest enclosing runtime.c frame chain.
        chain = []
        cur = indent
        for j in range(i, -1, -1):
            ind2, _, _, what2 = rows[j]
            if ind2 < cur or j == i:
                cur = ind2
                mm = re.search(r": (\w+) \((runtime\.c:\d+)\)", what2)
                if mm:
                    chain.append(f"{mm.group(1)} [{mm.group(2)}]")
        chain = [c for c in chain if not c.startswith(("goo_alloc", "goo_realloc"))]
        # massif's tree runs allocator-at-the-root, callers as children, so
        # walking UP from a leaf yields caller-first. Reverse it so this table
        # reads allocator <- caller, the same order the dhat table below uses.
        chain.reverse()
        if chain:
            leaves.append((nbytes, " <- ".join(chain[:2])))

agg = {}
for b, label in leaves:
    agg[label] = agg.get(label, 0) + b
total = sum(agg.values())
if total == 0:
    sys.exit("massif tree produced no attributable rows — check the parser")

print(f"{'B/req':>7} {'share':>7}  site")
for label, b in sorted(agg.items(), key=lambda kv: -kv[1]):
    if b / n < 1:
        continue
    print(f"{b/n:7.0f} {100*b/total:6.1f}%  {label}")
print(f"{total/n:7.0f} {100.0:6.1f}%  TOTAL LIVE")
PY

echo
echo "=== dhat: block counts (the retain/release traffic ARC would emit) ==="
valgrind --tool=dhat --dhat-out-file="$WORK/dhat.out" \
         "$WORK/daemon" "$REQ_DHAT" >/dev/null 2>&1 || fail "dhat run"

python3 - "$WORK/dhat.out" "$REQ_DHAT" <<'PY'
import json, re, sys
d = json.load(open(sys.argv[1]))
n = int(sys.argv[2])
ftbl = d["ftbl"]
rows = []

def walk(node):
    kids = node.get("pps", [])
    for c in kids:
        walk(c)
    if "tb" in node and not kids:
        rows.append((node["tb"], node["tbk"], [ftbl[i] for i in node.get("fs", [])]))

walk({"pps": d["pps"]})

def label(fs):
    names = []
    for f in fs:
        m = re.search(r": (\w+) \((runtime\.c:\d+)\)", f)
        if m:
            names.append(f"{m.group(1)} [{m.group(2)}]")
    keep = [x for x in names if not x.startswith(("goo_alloc", "goo_realloc"))]
    return " <- ".join(keep[:2]) if keep else (names[0] if names else "?")

agg = {}
for b, k, fs in rows:
    a = agg.setdefault(label(fs), [0, 0])
    a[0] += b
    a[1] += k

tb = sum(v[0] for v in agg.values())
tk = sum(v[1] for v in agg.values())
print(f"total allocated: {tb/n:.0f} B/req in {tk/n:.1f} blocks/req "
      f"(average payload {tb/tk - 16:.0f} B after the 16 B ARC header)")
print()
print(f"{'B/req':>7} {'blk/req':>8} {'share':>7}  site")
for lbl, (b, k) in sorted(agg.items(), key=lambda kv: -kv[1][0]):
    if b / n < 1:
        continue
    print(f"{b/n:7.0f} {k/n:8.1f} {100*b/tb:6.1f}%  {lbl}")
PY

echo
echo "daemon-alloc-attribution: DONE"
