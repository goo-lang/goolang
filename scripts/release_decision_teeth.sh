#!/usr/bin/env bash
# Does release_decision_test actually catch a missing condition?
#
# The suite passed on its FIRST run, which proves nothing: a test that has never
# failed has not shown it can. This matters more here than anywhere else in the
# ARC leg, because a wrong `RELEASE_OK` frees live memory.
#
# Each mutation DELETES one of the four conditions from `decide()` in
# src/types/release_decision.c and records which rows notice. A condition whose
# removal leaves the suite green is unguarded, and its rows are decoration.
#
# The mutation direction is UNSAFE on purpose: deleting a condition makes the
# module approve MORE locals, which is the direction that dangles a pointer. The
# engine is restored on every exit path, including a kill.
#
# Usage: scripts/release_decision_teeth.sh

set -uo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "FATAL: not in a git repository" >&2; exit 2; }
cd "$REPO_ROOT" || exit 2

SRC="src/types/release_decision.c"
WORK="${RELEASE_TEETH_WORKDIR:-$(mktemp -d)}"

die() { printf 'FATAL: %s\n' "$*" >&2; exit 2; }

# A COPY-BASED backup, and it VERIFIES. The first version of this script used
# `git checkout -- "$SRC" 2>/dev/null`, which silently did NOTHING because the
# file was UNTRACKED at the time. The redirect hid the error, every mutation
# stacked on the last, and the output still read plausibly -- all six conditions
# reported CAUGHT, with row counts that climbed monotonically because the
# deletions were cumulative. A restore that cannot fail loudly is not a restore.
BACKUP="$WORK/release_decision.c.orig"
cp -- "$SRC" "$BACKUP" || die "could not back up $SRC"

restore() {
    cp -- "$BACKUP" "$SRC" || { echo "FATAL: restore failed" >&2; return 1; }
    cmp -s -- "$BACKUP" "$SRC" || { echo "FATAL: restore did not match backup" >&2; return 1; }
}
trap restore EXIT INT TERM

# Each entry: label <TAB> the exact line to find <TAB> OPTIONAL replacement.
# With no third field the line is DELETED. With one, it is REPLACED, which is
# the only way to disable a rule whose line opens a braced block -- deleting
# such a line does not compile, and an INCONCLUSIVE-build reads almost like an
# unguarded condition.
MUTATIONS=(
"cond1-escapes	    if (local_escape_local_escapes(le, fn, r->name)) return RELEASE_NO_ESCAPES;"
"cond2-owned	    if (!binding_is_owned((Collected*)c, pe, r->bound_value)) return RELEASE_NO_NOT_OWNED;"
"cond3-arena	    if (r->arena_depth > 0) return RELEASE_NO_ARENA;"
"cond6-block-escape	        if (r->block_escapes) return RELEASE_NO_BLOCK_ESCAPE;"
# CONDITION 4'S LOOP HALF IS GONE, replaced by the two rules above and below.
# It read `if (r->loop_depth > 0) return RELEASE_NO_LOOP_SCOPE;` and refused
# EVERY local declared in a loop. Deleting that line is no longer an unsafe
# mutation: codegen releases a loop local at iteration end now, so the blanket
# refusal is not what keeps the module sound. The unsafe direction moved to the
# two rules that decide WHICH loop locals qualify, which is what these mutate.
"loop-header	        if (r->loop_header) return RELEASE_NO_LOOP_SCOPE;"
"cond4-rebound	    if (r->binding_count > 1) return RELEASE_NO_REBOUND;"
"unreadable	    if (c->unreadable) return RELEASE_NO_UNKNOWN;"
# Not in decide(): the SELF-APPEND rule lives in note_assignment. Without it
# `L = append(L, x)` counts as a rebind and row 18 must fail.
#
# THIS ENTRY WAS DEAD FROM PR #274 UNTIL 2026-08-01. That PR gave the rule a
# braced body (note_append_elems), so the one-line form it names stopped
# existing and the script died FATAL here -- before its last mutation, every
# time. Nothing noticed, because this script is not in verify-core. `&& false`
# rather than `if (false)` keeps every name referenced, so the mutant compiles
# without an unused-function warning that could mask the result.
"self-append	    if (plain_assign && is_self_append(name, rhs)) {	    if (plain_assign && is_self_append(name, rhs) && false) {"
)

# Baseline first. A red baseline makes every cell below meaningless.
make release_decision_test > "$WORK/build_base.log" 2>&1 \
    || die "baseline build failed; see $WORK/build_base.log"
./release_decision_test > "$WORK/run_base.log" 2>&1
base_rc=$?
grep -q "release_decision_test summary:" "$WORK/run_base.log" \
    || die "baseline produced no summary line"
[ "$base_rc" -eq 0 ] || die "baseline is RED; fix that before reading any mutation"
echo "baseline: PASS ($(grep -o 'summary:.*' "$WORK/run_base.log"))"
echo

rc=0
for entry in "${MUTATIONS[@]}"; do
    label="${entry%%$'\t'*}"
    rest="${entry#*$'\t'}"
    line="${rest%%$'\t'*}"
    # No third field means DELETE; `rest` then equals `line`.
    if [ "$rest" = "$line" ]; then replacement=""; else replacement="${rest#*$'\t'}"; fi

    # Assert the file is BACK to the original before mutating it again, or a
    # failed restore turns the next result into a cumulative lie.
    cmp -s -- "$BACKUP" "$SRC" || die "$SRC differs from the backup before mutation '$label'"
    grep -qF -- "$line" "$SRC" || die "mutation '$label' target line not found; the script is stale"
    python3 - "$SRC" "$line" "$replacement" <<'PY'
import sys
path, line, replacement = sys.argv[1], sys.argv[2], sys.argv[3]
src = open(path).read()
if src.count(line + "\n") != 1:
    sys.exit("target line is not unique")
new = (replacement + "\n") if replacement else ""
open(path, "w").write(src.replace(line + "\n", new, 1))
PY
    [ $? -eq 0 ] || die "could not apply mutation '$label'"

    if ! make release_decision_test > "$WORK/build_$label.log" 2>&1; then
        # A mutation that does not compile looks exactly like an unguarded
        # condition would if we only read the exit status, so say so plainly.
        printf '%-16s INCONCLUSIVE-build\n' "$label"
        rc=1
        restore
        continue
    fi
    ./release_decision_test > "$WORK/run_$label.log" 2>&1
    if ! grep -q "release_decision_test summary:" "$WORK/run_$label.log"; then
        printf '%-16s INCONCLUSIVE-crash\n' "$label"
        rc=1
        restore
        continue
    fi
    failed=$(grep -oP "summary: [0-9]+ assertions passed, \K[0-9]+" "$WORK/run_$label.log")
    rows=$(grep -c "^  Row .*: FAIL" "$WORK/run_$label.log")
    if [ "${failed:-0}" -gt 0 ]; then
        printf '%-16s CAUGHT   (%s assertions, %s rows)\n' "$label" "$failed" "$rows"
    else
        printf '%-16s UNGUARDED -- no row notices this condition missing\n' "$label"
        rc=1
    fi
    restore
done

echo
# Leave the tree exactly as found, and rebuild so the binary matches the source.
restore || die "final restore failed"
cmp -s -- "$BACKUP" "$SRC" || die "$SRC does not match the backup after restore"
make release_decision_test > "$WORK/build_restore.log" 2>&1 || die "restore build failed"
./release_decision_test > "$WORK/run_restore.log" 2>&1 \
    || die "suite is RED after restore; the tree was not restored cleanly"

if [ "$rc" -eq 0 ]; then
    echo "TEETH CONFIRMED: every condition has at least one row that catches its removal."
else
    echo "TEETH MISSING: see the UNGUARDED/INCONCLUSIVE lines above."
fi
exit "$rc"
