#!/usr/bin/env bash
# Grammar conflict tripwire: asserts the bison conflict baseline EXACTLY.
# Baseline history + justified-delta procedure: .claude/skills/goo-grammar/references/conflict-ledger.md
# Exit 0 = baseline exact. Exit 1 = delta (STOP: shape-diff + differential verification
# required before any commit). Exit 2 = bison itself failed.
set -u

EXPECTED_SR=31
EXPECTED_RR=0
PARSER=${1:-src/parser/parser.y}

TMPDIR_TW=$(mktemp -d)
trap 'rm -rf "$TMPDIR_TW"' EXIT

# Capture bison's own exit status directly — never through a pipeline.
OUT=$(bison -d -o "$TMPDIR_TW/p.tab.c" "$PARSER" 2>&1)
BISON_EXIT=$?
if [ "$BISON_EXIT" -ne 0 ]; then
    echo "grammar-tripwire: bison failed (exit $BISON_EXIT)"
    printf '%s\n' "$OUT"
    exit 2
fi

SR=$(printf '%s\n' "$OUT" | sed -n 's/.*warning: \([0-9][0-9]*\) shift\/reduce.*/\1/p')
RR=$(printf '%s\n' "$OUT" | sed -n 's/.*warning: \([0-9][0-9]*\) reduce\/reduce.*/\1/p')
SR=${SR:-0}
RR=${RR:-0}

if [ "$SR" -eq "$EXPECTED_SR" ] && [ "$RR" -eq "$EXPECTED_RR" ]; then
    echo "grammar-tripwire: PASS (${SR} S/R + ${RR} R/R — baseline exact)"
    exit 0
fi

echo "grammar-tripwire: FAIL — got ${SR} S/R + ${RR} R/R, baseline ${EXPECTED_SR}/${EXPECTED_RR}"
echo ""
echo "Counterexamples for the delta:"
bison -Wcounterexamples -d -o "$TMPDIR_TW/p2.tab.c" "$PARSER" 2>&1 | head -80
echo ""
echo "STOP: do not commit. Follow the justified-delta procedure in"
echo ".claude/skills/goo-grammar/references/conflict-ledger.md (classify via the"
echo "counterexamples, prove parse behavior via golden suite + targeted probes, then"
echo "update EXPECTED_SR/EXPECTED_RR here and the ledger IN THE SAME COMMIT)."
exit 1
