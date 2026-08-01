#!/usr/bin/env bash
# Does release_decision_test actually catch a missing condition?
#
# The suite passed on its FIRST run, which proves nothing: a test that has never
# failed has not shown it can. This matters more here than anywhere else in the
# ARC leg, because a wrong `RELEASE_OK` frees live memory.
#
# Each mutation DELETES one of the conditions from `decide()` in
# src/types/release_decision.c and records which rows notice. A condition whose
# removal leaves the suite green is unguarded, and its rows are decoration.
#
# The mutation direction is UNSAFE on purpose: deleting a condition makes the
# module approve MORE locals, which is the direction that dangles a pointer.
#
# THE TRACKED SOURCE IS NEVER WRITTEN. Earlier versions mutated it in place and
# restored it on every exit path, which was correct only while a human watched
# the run. This script is in verify-core now, so a `kill -9` between the write
# and the restore would leave the mutant on disk, and the NEXT build would
# compile it without a word. Each mutant goes to a scratch file instead, and
# `make ... RELEASE_DECISION_SRC=<mutant>` compiles that copy in its place.
#
# Usage: scripts/release_decision_teeth.sh

set -uo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "FATAL: not in a git repository" >&2; exit 2; }
cd "$REPO_ROOT" || exit 2

SRC="src/types/release_decision.c"
# verify-core runs this on every invocation, so a work directory per run would
# accumulate without bound. A directory this script created is removed when the
# run is GREEN, and kept when it is not, because the logs are the whole evidence
# for a red verdict. A caller-supplied one is never removed.
if [ -n "${RELEASE_TEETH_WORKDIR:-}" ]; then
    WORK="$RELEASE_TEETH_WORKDIR"
    WORK_IS_OURS=no
else
    WORK="$(mktemp -d)" || { echo "FATAL: could not make a work directory" >&2; exit 2; }
    WORK_IS_OURS=yes
fi
MAKE="${MAKE:-make}"
BIN=./release_decision_teeth_test
TEETH_OBJ=build/teeth/release_decision.o

# A FATAL exit keeps the work directory, so it must also say where it is. Every
# red path names its evidence; the reader should never have to hunt for it.
die() {
    printf 'FATAL: %s\n' "$*" >&2
    [ -d "${WORK:-}" ] && printf 'mutants and build/run logs kept in %s\n' "$WORK" >&2
    exit 2
}

# The property that replaced the restore: this script must leave the tracked
# source byte-identical. Asserted rather than assumed, because the whole design
# rests on it and a future edit could quietly reintroduce an in-place write.
[ -f "$SRC" ] || die "$SRC not found"
SRC_BEFORE="$(sha256sum -- "$SRC")" || die "could not checksum $SRC"

assert_src_untouched() {
    local now
    now="$(sha256sum -- "$SRC")" || die "could not re-checksum $SRC"
    [ "$now" = "$SRC_BEFORE" ] || die "$SRC WAS MODIFIED -- $1"
}

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
# CONDITION 4'S REBOUND HALF IS GONE, replaced by the two rules below. It read
# `if (r->binding_count > 1) return RELEASE_NO_REBOUND;` and refused EVERY
# rebound local. Deleting that line is no longer an unsafe mutation: codegen
# releases at the STORE now, so the blanket refusal is not what keeps the module
# sound. The unsafe direction moved to the two rules that decide WHICH rebound
# locals qualify, exactly as it did for the loop half before it.
#
# CONDITION 2'. One borrowed value among the recorded ones must refuse the whole
# local, because the store cannot tell the values apart at run time. Row 42.
"values-release-safe	        if (!all_values_release_safe((Collected*)c, pe, r)) return RELEASE_NO_NOT_OWNED;	        if (!all_values_release_safe((Collected*)c, pe, r) && false) return RELEASE_NO_NOT_OWNED;"
# CONDITION 7, the one nothing else provides. `p := last` holds a second pointer
# without making the value outlive the function, so local_escape reports nothing
# and condition 1 passes. Row 41.
"alias-refusal	        if (has_alias((Collected*)c, pe, r->name)) return RELEASE_NO_ALIASED;	        if (has_alias((Collected*)c, pe, r->name) && false) return RELEASE_NO_ALIASED;"
"unreadable	    if (c->unreadable) return RELEASE_NO_UNKNOWN;"
# Not in decide(): the SELF-APPEND rule lives in note_assignment. Without it
# `L = append(L, x)` counts as a rebind and row 18 must fail.
#
# THIS ENTRY WAS DEAD FROM PR #274 UNTIL 2026-08-01. That PR gave the rule a
# braced body (note_append_elems), so the one-line form it names stopped
# existing and the script died FATAL here -- before its last mutation, every
# time. Nothing noticed, because this script was not in verify-core. `&& false`
# rather than `if (false)` keeps every name referenced, so the mutant compiles
# without an unused-function warning that could mask the result.
"self-append	    if (plain_assign && is_self_append(name, rhs)) {	    if (plain_assign && is_self_append(name, rhs) && false) {"
)

# The table size is PINNED, in the shape of scripts/grammar-tripwire.sh's
# EXPECTED_SR. This is the #274 defect class, not a style rule: one mutation
# stopped running, and the summary line said exactly what it says when all of
# them run. A gate whose work can shrink to nothing while its verdict holds
# still is not a gate. Change this number in the same commit that adds or
# removes a mutation, and give the reason in the message.
EXPECTED_MUTATIONS=9
[ "${#MUTATIONS[@]}" -eq "$EXPECTED_MUTATIONS" ] \
    || die "mutation table holds ${#MUTATIONS[@]} entries, expected $EXPECTED_MUTATIONS"

# Build the mutant object from scratch every time. Without this the result
# depends on mtime comparison between a freshly written scratch file and the
# object left by the previous mutation, which is a race nobody should have to
# reason about for a gate that frees memory.
build_from() {
    rm -f "$TEETH_OBJ" "$BIN"
    "$MAKE" release_decision_teeth_test RELEASE_DECISION_SRC="$1" > "$2" 2>&1
}

# Baseline uses the SAME binary and the SAME link path as every mutation, with
# the UNMUTATED source. Building the ordinary release_decision_test here would
# leave a fault in this script's own build path undetected.
build_from "$SRC" "$WORK/build_base.log" \
    || die "baseline build failed; see $WORK/build_base.log"
"$BIN" > "$WORK/run_base.log" 2>&1
base_rc=$?
grep -q "release_decision_test summary:" "$WORK/run_base.log" \
    || die "baseline produced no summary line"
[ "$base_rc" -eq 0 ] || die "baseline is RED; fix that before reading any mutation"
echo "baseline: PASS ($(grep -o 'summary:.*' "$WORK/run_base.log"))"
echo

rc=0
caught=0
for entry in "${MUTATIONS[@]}"; do
    label="${entry%%$'\t'*}"
    rest="${entry#*$'\t'}"
    line="${rest%%$'\t'*}"
    # No third field means DELETE; `rest` then equals `line`.
    if [ "$rest" = "$line" ]; then replacement=""; else replacement="${rest#*$'\t'}"; fi

    mutant="$WORK/mut_$label.c"
    grep -qF -- "$line" "$SRC" || die "mutation '$label' target line not found; the script is stale"
    python3 - "$SRC" "$mutant" "$line" "$replacement" <<'PY'
import sys
src_path, dest_path, line, replacement = sys.argv[1:5]
src = open(src_path).read()
if src.count(line + "\n") != 1:
    sys.exit("target line is not unique")
new = (replacement + "\n") if replacement else ""
open(dest_path, "w").write(src.replace(line + "\n", new, 1))
PY
    [ $? -eq 0 ] || die "could not apply mutation '$label'"

    if ! build_from "$mutant" "$WORK/build_$label.log"; then
        # A mutation that does not compile looks exactly like an unguarded
        # condition would if we only read the exit status, so say so plainly.
        printf '%-18s INCONCLUSIVE-build\n' "$label"
        rc=1
        continue
    fi
    "$BIN" > "$WORK/run_$label.log" 2>&1
    if ! grep -q "release_decision_test summary:" "$WORK/run_$label.log"; then
        printf '%-18s INCONCLUSIVE-crash\n' "$label"
        rc=1
        continue
    fi
    failed=$(grep -oP "summary: [0-9]+ assertions passed, \K[0-9]+" "$WORK/run_$label.log")
    rows=$(grep -c "^  Row .*: FAIL" "$WORK/run_$label.log")
    if [ "${failed:-0}" -gt 0 ]; then
        printf '%-18s CAUGHT   (%s assertions, %s rows)\n' "$label" "$failed" "$rows"
        caught=$((caught + 1))
    else
        printf '%-18s UNGUARDED -- no row notices this condition missing\n' "$label"
        rc=1
    fi
    assert_src_untouched "during mutation '$label'"
done

echo
assert_src_untouched "by the run as a whole"
# The mutant object carries a mutation. Leaving it on disk would hand the next
# `make release_decision_teeth_test` a stale object for a source it no longer
# names, so remove both it and the binary.
rm -f "$TEETH_OBJ" "$BIN"

# CONFIRMED requires a POSITIVE count, not merely the absence of a complaint.
# "Nothing reported a problem" is the verdict an empty run gives too.
if [ "$caught" -ne "$EXPECTED_MUTATIONS" ]; then
    echo "TEETH MISSING: $caught of $EXPECTED_MUTATIONS conditions were caught."
    rc=1
elif [ "$rc" -eq 0 ]; then
    echo "TEETH CONFIRMED: all $EXPECTED_MUTATIONS conditions have a row that catches removal."
else
    echo "TEETH MISSING: see the UNGUARDED/INCONCLUSIVE lines above."
fi

if [ "$rc" -eq 0 ] && [ "$WORK_IS_OURS" = yes ]; then
    rm -rf -- "$WORK"
else
    echo "mutants and build/run logs kept in $WORK"
fi
exit "$rc"
