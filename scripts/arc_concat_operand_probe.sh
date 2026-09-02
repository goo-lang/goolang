#!/usr/bin/env bash
# Does codegen release a string-concat operand that nothing else names?
#
# `goo_string_concat` COPIES both operands and returns fresh memory, so an
# operand that is a fresh temporary is garbage the instant the call returns.
# examples/arc_concat_operand_probe.goo carries bench/daemon/daemon.goo's return
# line unchanged in `summary`, which leaks SIX records per call: two Itoa
# results, one Join result, and the three intermediate concats.
#
# THE ASSERTION IS SCOPED TO `summary` ON PURPOSE. The refusal functions in the
# same fixture leak a call result that `len()` consumes, which is the same
# general class with a DIFFERENT consumer and is not what this change fixes.
# Counting those here would make the probe go green on work it never did.
#
# The dangerous direction is the operand that is NOT a temporary: releasing one
# is a double free, because the local that names it releases the same buffer at
# function exit. That is what the valgrind-clean assertion catches, and the
# fixture carries a named, a borrowed, a view and a literal operand for it.
#
# Usage: scripts/arc_concat_operand_probe.sh

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
SRC="examples/arc_concat_operand_probe.goo"
WORK="${ARC_CONCAT_WORKDIR:-build}"
fail=0

[ -x "$COMPILER" ] || { echo "arc-concat-operand-probe: FAIL (no bin/goo)"; exit 1; }
[ -f "$SRC" ] || { echo "arc-concat-operand-probe: FAIL (no $SRC)"; exit 1; }
mkdir -p "$WORK"

if ! command -v valgrind >/dev/null 2>&1; then
    # Under make, tests.yml greps verify-core.log for the SKIPPED line and
    # fails the job, so the skip is loud there. A Bazel test log is read by
    # nobody: the harness sets GOO_PROBE_NO_SKIP, and the skip is the failure.
    if [ "${GOO_PROBE_NO_SKIP:-0}" = 1 ]; then
        echo "arc-concat-operand-probe: FAIL (valgrind not found, and GOO_PROBE_NO_SKIP forbids a skip)"
        exit 1
    fi
    echo "valgrind not found — SKIPPED"
    exit 0
fi

echo "=== arc-concat-operand-probe: releasing a concat operand nothing names ==="

GOO_ARC_RELEASE=0 "$COMPILER" -o "$WORK/arc_co_off" "$SRC" > "$WORK/arc_co_off.cerr" 2>&1 \
    || { echo "  FAIL (compile, release off)"; cat "$WORK/arc_co_off.cerr"; exit 1; }
"$COMPILER" -o "$WORK/arc_co_on" "$SRC" > "$WORK/arc_co_on.cerr" 2>&1 \
    || { echo "  FAIL (compile, release on)"; cat "$WORK/arc_co_on.cerr"; exit 1; }

# Bytes DEFINITELY LOST by loss records whose stack names `summary`. Anchored to
# the frame line valgrind prints for a call, never to the bare word, so a match
# is a stack frame and not a mention somewhere else in the log.
summary_lost() {
    python3 - "$1" <<'PY'
import re, sys
text = open(sys.argv[1], errors="replace").read()
total = 0
records = 0
pending = None
for line in text.splitlines():
    line = re.sub(r"^==\d+==\s?", "", line)
    m = re.match(r"\s*([\d,]+) bytes in [\d,]+ blocks are definitely lost", line)
    if m:
        pending = int(m.group(1).replace(",", ""))
        continue
    if pending is None:
        continue
    if re.search(r"^\s*by 0x[0-9A-Fa-f]+: summary \(", line):
        total += pending
        records += 1
        pending = None
    elif not line.strip():
        pending = None
print(f"{total} {records}")
PY
}

valgrind --leak-check=full --show-leak-kinds=all \
    "$WORK/arc_co_off" > "$WORK/arc_co_off.out" 2> "$WORK/arc_co_off.vg"
read -r lost_off recs_off <<< "$(summary_lost "$WORK/arc_co_off.vg")"

# SEVEN records, and the count is asserted rather than assumed. The bytes alone
# would let a fixture that stopped exercising an operand still read as a win.
if [ "${recs_off:-0}" -ne 7 ]; then
    echo "  FAIL: summary leaked $recs_off records with release off, expected 7 — the fixture changed"
    fail=1
elif [ "${lost_off:-0}" -eq 0 ]; then
    echo "  FAIL: with GOO_ARC_RELEASE=0 summary leaked nothing — the probe measures nothing"
    fail=1
else
    echo "  release OFF: $lost_off bytes in $recs_off records (6 operands + the returned value)"
fi

valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=none \
    --error-exitcode=99 \
    "$WORK/arc_co_on" > "$WORK/arc_co_on.out" 2> "$WORK/arc_co_on.vg"
rc=$?
read -r lost_on recs_on <<< "$(summary_lost "$WORK/arc_co_on.vg")"

# THE REFUSALS. A wrong release of a named, borrowed or view operand frees a
# buffer another owner still holds, and every one of those reads its bytes
# afterwards, so valgrind sees it as an invalid read or a double free.
if [ $rc -ne 0 ] || grep -qE "Invalid read|Invalid write|Invalid free|double free" "$WORK/arc_co_on.vg"; then
    echo "  FAIL: AN OPERAND THAT ANOTHER NAME HOLDS WAS RELEASED (rc=$rc)"
    grep -B2 -A8 -E "Invalid read|Invalid free|double free" "$WORK/arc_co_on.vg" | head -30
    fail=1
else
    echo "  refusals:    valgrind clean — named, borrowed, view and literal operands kept"
fi

# EXACTLY ONE record may survive: the value `summary` RETURNS. It is nobody's
# concat operand, so no rule in this change reaches it, and demanding zero here
# would make the probe fail over work it never claimed. Asserting the COUNT is
# what keeps that honest — a seventh record reappearing is a regression, and a
# residue of two means an operand stopped being released.
if [ "${recs_on:-9}" -ne 1 ]; then
    echo "  FAIL: summary keeps $recs_on records, expected 1 — an operand release is not firing"
    grep -A4 "definitely lost in loss record" "$WORK/arc_co_on.vg" | head -20
    fail=1
else
    echo "  reclaimed:   $lost_off -> $lost_on bytes, $recs_off -> $recs_on records"
    echo "  residue:     the value summary returns, which is no operand of any concat"
fi

if ! diff -q "$WORK/arc_co_off.out" "$WORK/arc_co_on.out" > /dev/null 2>&1; then
    echo "  FAIL: output differs between release OFF and ON"
    diff "$WORK/arc_co_off.out" "$WORK/arc_co_on.out" | head -10
    fail=1
else
    echo "  output:      identical with release off and on"
fi

if [ $fail -ne 0 ]; then echo "arc-concat-operand-probe: FAIL"; exit 1; fi
echo "arc-concat-operand-probe: PASS"
