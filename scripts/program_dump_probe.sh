#!/usr/bin/env bash
# program-dump-probe — bin/goo --emit-ast-json (parse stage) and
# --emit-program (typed stage) over every golden fixture:
#   1. the dump is produced (exit 0) for every fixture the stage can reach;
#   2. two runs are byte-identical (determinism);
#   3. scripts/program_dump_check.py accepts the structure.
# Any AST or type kind the walker does not handle aborts bin/goo with the
# kind's name, so a fixture that reaches an unhandled kind is a red row here
# and names what is missing. That is the coverage proof for the format.
#
# Teeth (--self-test): GOO_DUMP_SELFTEST=nonce makes the walker emit a
# per-process nonce, so the determinism check MUST fail; GOO_DUMP_SELFTEST=nopos
# drops the first node's pos, so the structural check MUST fail.
set -u
cd "$(dirname "$0")/.."
GOO=${COMPILER:-./bin/goo}
CHECK="python3 scripts/program_dump_check.py"

run_fixtures() {
    # $1 = stage flag; fixture paths on stdin, one per line
    local flag=$1 fails=0 n=0 tmp
    tmp=$(mktemp -d)
    while IFS= read -r f; do
        [ -z "$f" ] && continue
        n=$((n+1))
        local a="$tmp/a.json" b="$tmp/b.json"
        if ! "$GOO" "$flag" -o /dev/null "$f" > "$a" 2> "$tmp/err"; then
            echo "  FAIL  $f ($flag: exit $?)"; sed 's/^/        /' "$tmp/err" | head -3; fails=$((fails+1)); continue
        fi
        "$GOO" "$flag" -o /dev/null "$f" > "$b" 2>/dev/null
        if ! cmp -s "$a" "$b"; then echo "  FAIL  $f ($flag: two runs differ)"; fails=$((fails+1)); continue; fi
        if ! $CHECK "$a" > "$tmp/chk" 2>&1; then echo "  FAIL  $f ($flag: $(head -1 "$tmp/chk"))"; fails=$((fails+1)); continue; fi
    done
    rm -rf "$tmp"
    echo "  $flag: $n fixtures, $fails failed"
    return $((fails > 0))
}

parse_fixtures() {
    ls examples/*.expected.txt | sed 's/\.expected\.txt$/.goo/'
    ls tests/golden/reject/*.goo | grep -v -F -f <(grep -liE 'parse error|syntax error' tests/golden/reject/*.err.txt | sed 's/\.err\.txt$/.goo/')
}
typed_fixtures() { ls examples/*.expected.txt | sed 's/\.expected\.txt$/.goo/'; }

if [ "${1:-}" = "--self-test" ]; then
    f=examples/erru_catch_probe.goo
    tmp=$(mktemp -d)
    GOO_DUMP_SELFTEST=nonce "$GOO" --emit-ast-json -o /dev/null "$f" > "$tmp/1" 2>/dev/null
    GOO_DUMP_SELFTEST=nonce "$GOO" --emit-ast-json -o /dev/null "$f" > "$tmp/2" 2>/dev/null
    if cmp -s "$tmp/1" "$tmp/2"; then echo "program-dump-probe --self-test: FAIL (nonce did not change the dump)"; exit 1; fi
    GOO_DUMP_SELFTEST=nopos "$GOO" --emit-ast-json -o /dev/null "$f" > "$tmp/3" 2>/dev/null
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
