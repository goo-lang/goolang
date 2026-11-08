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
# GOO_DUMP_SELFTEST=badkind forces die_ast_kind on the first node emitted, so
# invariant 3 (every unhandled kind aborts by name) has an active tooth: the
# process must abort (exit >= 128) and name the kind on stderr. This also
# exercises the rc >= 128 branch below, which no other fixture ever takes.
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
    # Third tooth (fix round 1, finding 2): nonce and nopos above both run
    # against --emit-ast-json, the PARSE stage, which carries no type ids at
    # all -- nothing proved that a TYPED-stage node missing its type id is
    # actually refused. notype drops exactly one ordinary (non-callee)
    # IDENTIFIER/BINARY_EXPR node's "type" field; the checker must reject it.
    if ! GOO_DUMP_SELFTEST=notype "$GOO" --emit-program -o /dev/null "$f" > "$tmp/4" 2> "$tmp/err"; then
        echo "program-dump-probe --self-test: FAIL (dump of $f under notype failed: $(head -1 "$tmp/err"))"; exit 1
    fi
    if $CHECK "$tmp/4" >/dev/null 2>&1; then echo "program-dump-probe --self-test: FAIL (checker accepted a typed-stage node without a type id)"; exit 1; fi
    # Fourth tooth (invariant 3): badkind forces die_ast_kind on the first
    # node, which must abort (exit >= 128) and name the kind on stderr. The
    # regression this guards is a `case AST_X: break;` added to quiet a real
    # abort on some future fixture, which would drop the node silently and
    # leave the process exiting 0 -- so both checks below matter, not just
    # the exit status.
    GOO_DUMP_SELFTEST=badkind "$GOO" --emit-ast-json -o /dev/null "$f" > "$tmp/5" 2> "$tmp/err5"
    rc=$?
    if [ "$rc" -lt 128 ]; then echo "program-dump-probe --self-test: FAIL (badkind exited $rc, not an abort)"; exit 1; fi
    grep -q "program-dump: unsupported AST node kind" "$tmp/err5" || { echo "program-dump-probe --self-test: FAIL (badkind did not name the kind on stderr)"; exit 1; }
    rm -rf "$tmp"
    echo "program-dump-probe --self-test: PASS (nonce breaks determinism, missing pos is refused, missing type id is refused, badkind aborts with a synthetic out-of-range kind)"
    exit 0
fi

ok=1
echo "=== program-dump-probe: dump every fixture at both stages ==="
parse_fixtures | run_fixtures --emit-ast-json || ok=0
typed_fixtures | run_fixtures --emit-program || ok=0

# The ARC kill switch (GOO_ARC_RELEASE=0) is a documented valid typed-stage
# configuration: goo.c never calls release_plan_analyze under it, so every
# file's "plan" is null instead of the list emit_plan otherwise writes
# (format spec, "The release plan"). Neither run_fixtures call above sets
# this -- both dump with ARC on -- so a checker regression on a null
# typed-stage plan would stay dormant until someone diffs an ARC-off dump
# by hand. One fixture exercises the path; the point is coverage, not a
# second full sweep.
arc_off_fixture=examples/erru_catch_probe.goo
arc_off_tmp=$(mktemp)
if ! GOO_ARC_RELEASE=0 "$GOO" --emit-program -o /dev/null "$arc_off_fixture" > "$arc_off_tmp" 2>/dev/null; then
    echo "  FAIL  $arc_off_fixture (--emit-program under GOO_ARC_RELEASE=0: dump failed)"; ok=0
elif ! $CHECK "$arc_off_tmp" >/dev/null 2>&1; then
    echo "  FAIL  $arc_off_fixture (--emit-program under GOO_ARC_RELEASE=0: checker refused a null plan)"; ok=0
else
    echo "  --emit-program (GOO_ARC_RELEASE=0): $arc_off_fixture accepted, plan: null"
fi
rm -f "$arc_off_tmp"

if [ $ok = 1 ]; then echo "program-dump-probe: PASS"; else echo "program-dump-probe: FAIL"; exit 1; fi
