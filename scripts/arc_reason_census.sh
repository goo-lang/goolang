#!/usr/bin/env bash
# How far does each escape reason reach across the corpus?
#
# NOT A GATE. A measurement, run by hand, so that a claim about the CALLEE_VALUE
# ceiling comes from a count rather than from repetition. `include/escape_core.h`
# says naming that ceiling does not lift it but does make it MEASURABLE, and
# this is the thing that measures it.
#
# THE TRAP THIS SCRIPT EXISTS TO AVOID. A miss reads as ESCAPE_REASON_ALL, which
# is the fail-closed answer local_escape.h requires -- a parameter, an unknown
# function, or an unanalysed local all report every reason at once. Counting
# "refusals mentioning CALLEE_VALUE" without excluding those attributes every
# miss in the tree to the ceiling and inflates it enormously. ALL is counted as
# UNANALYSED here, never as a reason.
#
# Usage: ./scripts/arc_reason_census.sh [outfile]
set -u

GOO=${GOO:-./bin/goo}
OUT=${1:-/dev/stdout}
[ -x "$GOO" ] || { echo "no $GOO -- run make lexer first" >&2; exit 2; }

ALL_NAMES='UNCLASSIFIED|RETURN|GLOBAL_STORE|CONTAINER_STORE|SUBSCRIPT_STORE|CALL_RETAIN|CALLEE_VALUE|GO_ARG|DEFER_ARG|CHAN_SEND|CLOSURE_CAPTURE|CALL_OPAQUE|CALL_VARIADIC'

RAW=$(mktemp); trap 'rm -f "$RAW"' EXIT
progs=0; failed=0

for f in examples/*.goo bench/*/*.goo; do
    [ -f "$f" ] || continue
    progs=$((progs + 1))
    # A program that does not compile still contributes nothing, and that is
    # reported rather than hidden -- a shrinking corpus reads as a clean result.
    if ! GOO_ARC_DEBUG=1 "$GOO" -o /dev/null "$f" 2>&1 \
         | grep '^\[arc?\]' | sed "s|^|$f\t|" >> "$RAW"; then
        failed=$((failed + 1))
    fi
done

# Fields: file, function, local, verdict, reasons
awk -F'\t' -v ALLN="$ALL_NAMES" '
{
    line = $2
    # [arc?] FN: LOCAL -> VERDICT (reasons=SET)
    match(line, /\] [^:]+:/);       fn  = substr(line, RSTART+2, RLENGTH-3)
    match(line, /: [^ ]+ ->/);      loc = substr(line, RSTART+2, RLENGTH-5)
    match(line, /-> [A-Z_]+/);      vd  = substr(line, RSTART+3, RLENGTH-3)
    match(line, /reasons=[^)]*/);   rs  = substr(line, RSTART+8, RLENGTH-8)

    user = (fn !~ /^goo_pkg__/)     # vendored goostd is re-analysed per program

    total++;              if (user) utotal++
    # BEFORE any `next` below, because several arms skip the rest of the body
    # and a verdict counted after one of them would silently under-report.
    vd_cnt[vd]++;         if (user) uvd_cnt[vd]++
    if (vd == "RELEASE_OK") { ok++; if (user) uok++; next }

    refused++;            if (user) urefused++
    if (rs == ALLN) { unanalysed++; if (user) uunanalysed++; next }

    attributed++;         if (user) uattributed++
    if (rs == "NONE") { none++; if (user) unone++ }
    n = split(rs, parts, "|")
    for (i = 1; i <= n; i++) { if (parts[i] != "NONE") { cnt[parts[i]]++; if (user) ucnt[parts[i]]++ } }
}
END {
    printf "corpus rows: %d (user-code rows: %d)\n\n", total, utotal
    printf "%-34s %10s %10s\n", "", "ALL", "USER-CODE"
    printf "%-34s %10d %10d\n", "released (RELEASE_OK)",        ok,         uok
    printf "%-34s %10d %10d\n", "refused",                      refused,    urefused
    printf "%-34s %10d %10d\n", "  ...UNANALYSED (reasons=ALL)",unanalysed, uunanalysed
    printf "%-34s %10d %10d\n", "  ...refused with a real set", attributed, uattributed
    printf "%-34s %10d %10d\n", "     of those, reasons=NONE",  none,       unone
    printf "\nreason tallies (a local may carry several; misses excluded)\n"
    for (r in cnt) printf "%-34s %10d %10d\n", "  " r, cnt[r], ucnt[r]

    # VERDICT TALLY. Added 2026-08-03, and it is a different question from the
    # reason tally above: a REASON says why the escape analysis marked a local,
    # a VERDICT says which condition in release_decision refused it. Only
    # (NO APOSTROPHE ANYWHERE IN THIS BLOCK. The awk program is a single-quoted
    # shell string, so one apostrophe ends it and bash then tries to run awk
    # syntax. That is what happened when this comment was first written.)
    # RELEASE_OK appeared anywhere in this output before, so the relative size
    # of the conditions was unmeasured -- and .handoff.md item 4 planned work
    # against a verdict this tally shows is reported ZERO times in the corpus
    # (RELEASE_NO_REBOUND, which has no producer in src/ at all).
    #
    # One row per local, so unlike the reason tally these DO sum to the total.
    printf "\nverdict tallies (one per local; these sum to the row count)\n"
    for (v in vd_cnt) printf "%-34s %10d %10d\n", "  " v, vd_cnt[v], uvd_cnt[v]
}' "$RAW" | tee "$OUT"

echo
echo "programs compiled: $progs, of which failed: $failed"
