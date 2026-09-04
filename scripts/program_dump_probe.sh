#!/usr/bin/env bash
# program-dump-probe — bin/goo --emit-ast-json (parse stage) and
# --emit-program (typed stage) over every golden fixture:
#   1. the dump is produced (exit 0) for every fixture the stage can reach;
#   2. two runs are byte-identical (determinism);
#   3. scripts/program_dump_check.py accepts the structure.
#
# A reject fixture (tests/golden/reject) may instead be REFUSED before the
# stage is reached: a non-zero exit whose stderr contains the fixture's
# .err.txt text is the golden rejection reproduced, and counts as "rejected
# as golden", not as a failure. That is the rule scripts/run_golden_reject.sh
# applies, so this probe needs no list of which rejects are parse errors.
#
# An exit of 128 or more is an abort and is always a failure. Any AST or type
# kind the walker does not handle aborts bin/goo naming the kind, so a fixture
# that reaches an unhandled kind is a red row here that names what is missing.
# That is the coverage proof for the format.
#
# Teeth (--self-test): GOO_DUMP_SELFTEST=nonce makes the walker emit a
# per-process nonce, so the determinism check MUST fail; GOO_DUMP_SELFTEST=nopos
# drops the first node's pos, so the structural check MUST fail. Both need the
# fixture to dump at all, so a dump failure is reported as such, never as
# "the nonce did not change the dump" (an abort leaves both files empty).
set -u
cd "$(dirname "$0")/.."
GOO=${COMPILER:-./bin/goo}
CHECK="python3 scripts/program_dump_check.py"

# Exit 0 when $1 is a reject fixture and the stderr saved in $2 carries the
# text of its .err.txt sidecar: the golden rejection, reproduced.
golden_rejection() {
    local f=$1 err=$2 sidecar
    case "$f" in tests/golden/reject/*) ;; *) return 1 ;; esac
    sidecar=${f%.goo}.err.txt
    [ -s "$sidecar" ] || return 1
    grep -qF -- "$(cat "$sidecar")" "$err"
}

run_fixtures() {
    # $1 = stage flag; fixture paths on stdin, one per line
    local flag=$1 fails=0 n=0 rejected=0 rc tmp
    tmp=$(mktemp -d)
    while IFS= read -r f; do
        [ -z "$f" ] && continue
        n=$((n+1))
        local a="$tmp/a.json" b="$tmp/b.json"
        "$GOO" "$flag" -o /dev/null "$f" > "$a" 2> "$tmp/err"
        rc=$?
        if [ "$rc" -ge 128 ]; then
            echo "  FAIL  $f ($flag: exit $rc, aborted)"; sed 's/^/        /' "$tmp/err" | head -3
            fails=$((fails+1)); continue
        fi
        if [ "$rc" -ne 0 ]; then
            if golden_rejection "$f" "$tmp/err"; then rejected=$((rejected+1)); continue; fi
            echo "  FAIL  $f ($flag: exit $rc)"; sed 's/^/        /' "$tmp/err" | head -3
            fails=$((fails+1)); continue
        fi
        "$GOO" "$flag" -o /dev/null "$f" > "$b" 2>/dev/null
        if ! cmp -s "$a" "$b"; then echo "  FAIL  $f ($flag: two runs differ)"; fails=$((fails+1)); continue; fi
        if ! $CHECK "$a" > "$tmp/chk" 2>&1; then echo "  FAIL  $f ($flag: $(head -1 "$tmp/chk"))"; fails=$((fails+1)); continue; fi
    done
    rm -rf "$tmp"
    if [ "$n" -eq 0 ]; then echo "  FAIL  $flag: no fixtures found"; return 1; fi
    echo "  $flag: $n fixtures, $rejected rejected as golden, $fails failed"
    return $((fails > 0))
}

parse_fixtures() {
    ls examples/*.expected.txt | sed 's/\.expected\.txt$/.goo/'
    ls tests/golden/reject/*.goo
}
typed_fixtures() { ls examples/*.expected.txt | sed 's/\.expected\.txt$/.goo/'; }

if [ "${1:-}" = "--self-test" ]; then
    f=examples/erru_catch_probe.goo
    tmp=$(mktemp -d)
    for i in 1 2; do
        if ! GOO_DUMP_SELFTEST=nonce "$GOO" --emit-ast-json -o /dev/null "$f" > "$tmp/$i" 2> "$tmp/err"; then
            echo "program-dump-probe --self-test: FAIL (dump of $f failed: $(head -1 "$tmp/err"))"; exit 1
        fi
    done
    if cmp -s "$tmp/1" "$tmp/2"; then echo "program-dump-probe --self-test: FAIL (nonce did not change the dump)"; exit 1; fi
    if ! GOO_DUMP_SELFTEST=nopos "$GOO" --emit-ast-json -o /dev/null "$f" > "$tmp/3" 2> "$tmp/err"; then
        echo "program-dump-probe --self-test: FAIL (dump of $f failed: $(head -1 "$tmp/err"))"; exit 1
    fi
    if $CHECK "$tmp/3" >/dev/null 2>&1; then echo "program-dump-probe --self-test: FAIL (checker accepted a node without pos)"; exit 1; fi
    rm -rf "$tmp"
    echo "program-dump-probe --self-test: PASS (nonce breaks determinism, missing pos is refused)"
    exit 0
fi

ok=1
echo "=== program-dump-probe: dump every fixture at both stages ==="
parse_fixtures | run_fixtures --emit-ast-json || ok=0
typed_fixtures | run_fixtures --emit-program || ok=0
if [ $ok = 1 ]; then echo "program-dump-probe: PASS"; else echo "program-dump-probe: FAIL"; exit 1; fi
