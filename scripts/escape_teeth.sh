#!/usr/bin/env bash
# Do param/block/local_escape_test actually catch a missing condition?
#
# All three suites run in verify-core and all three passed on their FIRST run,
# which proves nothing: a test that has never failed has not shown it can.
#
# scripts/escape_arm_coverage.sh already mutates FOR these three suites, but it
# mutates src/types/escape_core.c — the SHARED taint engine. Each pass also owns
# code that no gate touched before this script:
#
#   param_escape.c   the interprocedural return signal, the parameter self-bit,
#                    the registry propagation that carries a callee summary back
#   block_escape.c   alloc-site discovery (new(T), &T{}), the site self-bit, and
#                    defer_is_like_go = TRUE, the one place the block boundary
#                    must diverge from param_escape
#   local_escape.c   the local self-bit (the ONLY place a local becomes a
#                    source), the `_` skip, the name-to-slot match
#
# Each mutation below disables ONE such condition and records which rows notice.
# A condition whose removal leaves a suite green is unguarded, and its rows are
# decoration.
#
# DIRECTION. All three public lookups fail CLOSED (`return true` on a miss), so
# the two mutation directions are caught by two different row populations:
#
#   a broken MARKING condition under-marks -> only a SOUNDNESS row (expects
#   escapes=true) can catch it. This is the direction that dangles a pointer,
#   because T4 then frees memory the program still reads.
#
#   a broken DISCOVERY condition over-marks -> only a PRECISION row (expects
#   escapes=false) can catch it. This direction cannot make the compiler unsafe;
#   it silently costs reclamation.
#
# Both are worth teeth, so the tables carry both, and each entry says which it
# is. An UNGUARDED verdict on a precision entry means the suite has no row that
# would notice the pass going conservative.
#
# THE TRACKED SOURCE IS NEVER WRITTEN. This is the discipline
# release_decision_teeth.sh adopted and escape_arm_coverage.sh still lacks: that
# script edits the tracked engine and restores it from a trap, and its own header
# records the run where that trap DESTROYED uncommitted work. A gate that
# verify-core may run must not be able to leave a mutant on disk, so each mutant
# goes to a scratch file and `make ... <PASS>_ESCAPE_SRC=<mutant>` compiles that
# copy in place of the real object.
#
# Usage:
#   scripts/escape_teeth.sh --self-test    # prove the harness can say BOTH
#   scripts/escape_teeth.sh                # all three passes
#   scripts/escape_teeth.sh local          # one pass

set -uo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "FATAL: not in a git repository" >&2; exit 2; }
cd "$REPO_ROOT" || exit 2

MAKE="${MAKE:-make}"
ALL_MODULES=(param block local)

# verify-core would run this on every invocation, so a work directory per run
# would accumulate without bound. A directory this script created is removed when
# the run is GREEN and kept when it is not, because the logs are the whole
# evidence for a red verdict. A caller-supplied one is never removed.
if [ -n "${ESCAPE_TEETH_WORKDIR:-}" ]; then
    WORK="$ESCAPE_TEETH_WORKDIR"
    WORK_IS_OURS=no
else
    WORK="$(mktemp -d)" || { echo "FATAL: could not make a work directory" >&2; exit 2; }
    WORK_IS_OURS=yes
fi

# A FATAL exit keeps the work directory, so it must also say where it is. Every
# red path names its evidence; the reader should never have to hunt for it.
die() {
    printf 'FATAL: %s\n' "$*" >&2
    [ -d "${WORK:-}" ] && printf 'mutants and build/run logs kept in %s\n' "$WORK" >&2
    exit 2
}

# ---------------------------------------------------------------------------
# Mutation tables. Each entry: label <TAB> the exact line to find <TAB> OPTIONAL
# replacement. With no third field the line is DELETED. With one, it is
# REPLACED, which is the only way to disable a rule whose line opens a braced
# block — deleting such a line does not compile, and an INCONCLUSIVE-build reads
# almost like an unguarded condition.
#
# `&& false` rather than deletion is also used where deletion would leave a
# variable unreferenced. CFLAGS carries -Wall -Wextra but NOT -Werror, so such a
# warning would not break the build today; the form is kept anyway so a future
# -Werror cannot silently turn these entries into INCONCLUSIVE-build.
# ---------------------------------------------------------------------------

# SOUNDNESS x4, PRECISION x1.
PARAM_MUTATIONS=(
# The interprocedural return signal: F returns a value derived from its own
# parameter. Suppressed here, every caller of such an F under-marks.
"on-return	    if (!escape_taint_empty(value_taint)) *own->return_escapes = true;	    if (!escape_taint_empty(value_taint) && false) *own->return_escapes = true;"
# The parameter self-bit. Without it a parameter is never a taint source, so
# nothing in the pass can ever be traced back to one.
"param-self-bit	        seed.bits[i] = true;"
# The callee's return-escape summary, read out of the in-progress registry.
# Forced false, a caller believes no callee ever hands back its own parameter.
"retention-return	    *out_return_escapes = callee->return_escapes;	    *out_return_escapes = false;"
# The outer fixpoint's propagation into the registry. Suppressed, a param that
# escapes is computed correctly and then never published to its callers.
"escapes-propagate	                if (local_escapes[i] && !f->escapes[i]) {	                if (local_escapes[i] && !f->escapes[i] && false) {"
# PRECISION. At FUNCTION granularity a defer runs inside the frame, so `false`
# is the precise answer and `true` is merely conservative. Only a row that
# expects a deferred argument NOT to escape can notice.
"defer-precision	    .defer_is_like_go = false,	    .defer_is_like_go = true,"
)
PARAM_EXPECTED=5

# SOUNDNESS x3, PRECISION x2.
BLOCK_MUTATIONS=(
# PRECISION. new(T) stops being discovered as an alloc site. An undiscovered
# site answers `true` from the public lookup, so only a row that expects a
# new(T) inside an arena block NOT to escape can notice.
"discover-new	    if (is_new_call(expr)) {	    if (is_new_call(expr) && false) {"
# PRECISION. Same for &T{}.
"discover-addr-composite	    if (is_addr_of_composite(expr)) {	    if (is_addr_of_composite(expr) && false) {"
# The site self-bit hook. Without it no alloc site is a taint source, so no
# site can ever be traced to a sink and every one reads non-escaping.
"site-source-slot	    .expr_source_slot = block_expr_source_slot,	    .expr_source_slot = NULL,"
# The site-to-slot match. A site that resolves to no slot is never seeded.
"site-slot-miss	    if (idx == ESCAPE_NO_SLOT) return false;	    if (idx != ESCAPE_NO_SLOT) return false;"
# THE ONE PLACE THE BLOCK BOUNDARY MUST DIVERGE. A defer runs at the enclosing
# FUNCTION's exit, always after this arena block closed and freed. Flipped to
# false, a deferred argument is arena-freed before the deferred call reads it.
"defer-is-like-go	    .defer_is_like_go = true,	    .defer_is_like_go = false,"
# NO retention-return ENTRY HERE, unlike the param table. Not an oversight --
# MEASURED as impossible to catch, and param_escape_test row 26 is the standing
# proof. Sink #1 marks a returned value before on_return records the signal, so
# a callee with return_escapes=true ALWAYS has escapes[i]=true too. At a call
# site `retains` reads escapes[i], so the argument is marked by sink #5 whatever
# return_escapes says, and forcing it false changes no verdict. Adding the entry
# back would make this gate permanently red for a condition no fixture can
# reach, which teaches readers to ignore UNGUARDED. See block row 33.
)
BLOCK_EXPECTED=5

# SOUNDNESS x3, PRECISION x0 -- see the two dropped entries noted below for why
# this table is the shortest of the three.
LOCAL_MUTATIONS=(
# The local self-bit — the ONLY place a local becomes a taint source. Without
# it no local is ever traced to a sink, and every one reads non-escaping.
"local-self-bit	    if (idx < seeded.n) seeded.bits[idx] = true;	    if (idx < seeded.n && false) seeded.bits[idx] = true;"
# The name-to-slot match. Every lookup misses, so no local is ever seeded.
"local-slot-match	        if (own->local_names[i] && strcmp(own->local_names[i], name) == 0) return i;	        if (own->local_names[i] && strcmp(own->local_names[i], name) != 0) return i;"
# NO retention-return ENTRY, for the reason the block table gives above:
# measured unobservable, with param_escape_test row 26 as the proof.
#
# NO blank-skip ENTRY EITHER. Registering `_` as an ordinary local was measured
# to change NO verdict: it takes a slot ahead of its neighbours, but every
# local's own bit and its name-to-slot lookup move together, so the shift
# cancels. `_` is never read, so its bit reaches no sink. The skip is a tidiness
# guard, not a correctness rule, and local row 35 is the fixture that measured
# this. A mutation no row can ever catch does not belong in a gate.
#
# This pass's boundary is the FUNCTION, so param_escape's `false` is the precise
# answer and this `true` is deliberately conservative pending a decision on
# release ordering against deferred calls. Flipping it is the direction that
# would dangle, so it is a soundness entry despite guarding precision today.
"defer-is-like-go	    .defer_is_like_go = true,	    .defer_is_like_go = false,"
)
LOCAL_EXPECTED=3

# The table sizes are PINNED, in the shape of scripts/grammar-tripwire.sh's
# EXPECTED_SR. This is the #274 defect class, not a style rule: there, one
# mutation stopped running and the summary line said exactly what it says when
# all of them run. A gate whose work can shrink to nothing while its verdict
# holds still is not a gate. Change these numbers in the same commit that adds
# or removes a mutation, and give the reason in the message.

# ---------------------------------------------------------------------------
# The property that replaces an in-place restore: this script leaves every
# tracked source byte-identical. Asserted rather than assumed, because the whole
# design rests on it and a future edit could quietly reintroduce a write.
# ---------------------------------------------------------------------------
declare -A SRC_BEFORE
for m in "${ALL_MODULES[@]}"; do
    src="src/types/${m}_escape.c"
    [ -f "$src" ] || die "$src not found"
    SRC_BEFORE[$m]="$(sha256sum -- "$src")" || die "could not checksum $src"
done

assert_sources_untouched() {
    local m src now
    for m in "${ALL_MODULES[@]}"; do
        src="src/types/${m}_escape.c"
        now="$(sha256sum -- "$src")" || die "could not re-checksum $src"
        [ "$now" = "${SRC_BEFORE[$m]}" ] || die "$src WAS MODIFIED -- $1"
    done
}

# ---------------------------------------------------------------------------
# One build/run/verdict cycle.
#
# Every verdict is read from the suite's own `summary:` line, never from an exit
# status alone. A mutation that does not compile, and one that crashes, both
# look exactly like a caught condition if only the status is read — and a run
# with no summary line at all is INCONCLUSIVE, not a pass.
#
# Sets VERDICT and DETAIL. Returns 0 always; the caller judges.
# ---------------------------------------------------------------------------
VERDICT=""
DETAIL=""

build_from() {  # module, source-to-compile, logfile
    local m="$1" src="$2" log="$3"
    local upper; upper="$(printf '%s' "$m" | tr '[:lower:]' '[:upper:]')"
    # Build the mutant object from scratch every time. Without this the result
    # depends on mtime comparison between a freshly written scratch file and the
    # object left by the previous mutation, which is a race nobody should have to
    # reason about for a gate whose consumer frees memory.
    rm -f "build/teeth/${m}_escape.o" "./${m}_escape_teeth_test"
    "$MAKE" "${m}_escape_teeth_test" "${upper}_ESCAPE_SRC=$src" > "$log" 2>&1
}

run_one() {  # module, label, line, replacement (may be empty)
    local m="$1" label="$2" line="$3" replacement="$4"
    local src="src/types/${m}_escape.c"
    local mutant="$WORK/mut_${m}_${label}.c"
    local bin="./${m}_escape_teeth_test"

    grep -qF -- "$line" "$src" \
        || die "mutation '$m/$label' target line not found; the script is stale"

    python3 - "$src" "$mutant" "$line" "$replacement" <<'PY'
import sys
src_path, dest_path, line, replacement = sys.argv[1:5]
src = open(src_path).read()
if src.count(line + "\n") != 1:
    sys.exit("target line is not unique")
new = (replacement + "\n") if replacement else ""
open(dest_path, "w").write(src.replace(line + "\n", new, 1))
PY
    [ $? -eq 0 ] || die "could not apply mutation '$m/$label'"

    if ! build_from "$m" "$mutant" "$WORK/build_${m}_${label}.log"; then
        VERDICT="INCONCLUSIVE-build"
        DETAIL="see $WORK/build_${m}_${label}.log"
        return 0
    fi
    "$bin" > "$WORK/run_${m}_${label}.log" 2>&1
    if ! grep -q "${m}_escape_test summary:" "$WORK/run_${m}_${label}.log"; then
        VERDICT="INCONCLUSIVE-crash"
        DETAIL="see $WORK/run_${m}_${label}.log"
        return 0
    fi
    local failed rows
    failed=$(grep -oP "summary: [0-9]+ assertions passed, \K[0-9]+" "$WORK/run_${m}_${label}.log")
    rows=$(grep -c "^  Row .*: FAIL" "$WORK/run_${m}_${label}.log")
    if [ "${failed:-0}" -gt 0 ]; then
        VERDICT="CAUGHT"
        DETAIL="$failed assertions, $rows rows"
    else
        VERDICT="UNGUARDED"
        DETAIL="no row notices this condition missing"
    fi
    return 0
}

# ---------------------------------------------------------------------------
# --self-test: prove this harness can report the OPPOSITE result.
#
# A mutation matrix that can only ever print CAUGHT is not an instrument. All
# three controls target the SAME line in local_escape.c, so any difference in
# verdict comes from the mutation and nothing else.
# ---------------------------------------------------------------------------
SELF_TEST_LINE='    if (idx < seeded.n) seeded.bits[idx] = true;'

self_test() {
    local rc=0
    echo "SELF-TEST: can this harness report more than one verdict?"
    echo

    # Control 1: a real mutation. The local self-bit is the only place a local
    # becomes a source, so suppressing it must break soundness rows.
    run_one local ctl-caught "$SELF_TEST_LINE" \
        "    if (idx < seeded.n && false) seeded.bits[idx] = true;"
    printf '  %-14s expect CAUGHT              got %-20s %s\n' "ctl-caught" "$VERDICT" "$DETAIL"
    [ "$VERDICT" = "CAUGHT" ] || { echo "  SELF-TEST FAILED: a real mutation was not caught"; rc=1; }

    # Control 2: a NO-OP. The line is replaced with itself, so behaviour is
    # unchanged and the suite must stay green. This is the control that proves
    # the harness does not simply print CAUGHT for everything.
    run_one local ctl-noop "$SELF_TEST_LINE" "$SELF_TEST_LINE"
    printf '  %-14s expect UNGUARDED           got %-20s %s\n' "ctl-noop" "$VERDICT" "$DETAIL"
    [ "$VERDICT" = "UNGUARDED" ] || { echo "  SELF-TEST FAILED: a no-op did not read UNGUARDED"; rc=1; }

    # Control 3: does not compile. Must be told apart from an unguarded
    # condition, which is what it resembles when only an exit status is read.
    run_one local ctl-nobuild "$SELF_TEST_LINE" "    this is not valid c;"
    printf '  %-14s expect INCONCLUSIVE-build  got %-20s %s\n' "ctl-nobuild" "$VERDICT" "$DETAIL"
    [ "$VERDICT" = "INCONCLUSIVE-build" ] || { echo "  SELF-TEST FAILED: a broken build did not read INCONCLUSIVE-build"; rc=1; }

    echo
    assert_sources_untouched "by the self-test"
    if [ "$rc" -eq 0 ]; then
        echo "SELF-TEST PASSED: the harness reports CAUGHT, UNGUARDED and INCONCLUSIVE-build."
    else
        echo "SELF-TEST FAILED: trust no verdict from this script until it is fixed."
    fi
    return "$rc"
}

# ---------------------------------------------------------------------------
# One pass's full matrix.
# ---------------------------------------------------------------------------
run_module() {
    local m="$1"
    local -n table="$(printf '%s' "$m" | tr '[:lower:]' '[:upper:]')_MUTATIONS"
    local expected_var; expected_var="$(printf '%s' "$m" | tr '[:lower:]' '[:upper:]')_EXPECTED"
    local expected="${!expected_var}"
    local src="src/types/${m}_escape.c"
    local rc=0 caught=0

    [ "${#table[@]}" -eq "$expected" ] \
        || die "$m table holds ${#table[@]} entries, expected $expected"

    # Baseline uses the SAME binary and the SAME link path as every mutation,
    # with the UNMUTATED source. Building the ordinary suite here would leave a
    # fault in this script's own build path undetected.
    build_from "$m" "$src" "$WORK/build_${m}_base.log" \
        || die "$m baseline build failed; see $WORK/build_${m}_base.log"
    "./${m}_escape_teeth_test" > "$WORK/run_${m}_base.log" 2>&1
    local base_rc=$?
    grep -q "${m}_escape_test summary:" "$WORK/run_${m}_base.log" \
        || die "$m baseline produced no summary line"
    [ "$base_rc" -eq 0 ] || die "$m baseline is RED; fix that before reading any mutation"
    echo "== $m =="
    echo "baseline: PASS ($(grep -o 'summary:.*' "$WORK/run_${m}_base.log"))"

    local entry label rest line replacement
    for entry in "${table[@]}"; do
        label="${entry%%$'\t'*}"
        rest="${entry#*$'\t'}"
        line="${rest%%$'\t'*}"
        # No third field means DELETE; `rest` then equals `line`.
        if [ "$rest" = "$line" ]; then replacement=""; else replacement="${rest#*$'\t'}"; fi

        run_one "$m" "$label" "$line" "$replacement"
        case "$VERDICT" in
            CAUGHT)
                printf '  %-26s CAUGHT   (%s)\n' "$label" "$DETAIL"
                caught=$((caught + 1)) ;;
            UNGUARDED)
                printf '  %-26s UNGUARDED -- %s\n' "$label" "$DETAIL"
                rc=1 ;;
            *)
                printf '  %-26s %s (%s)\n' "$label" "$VERDICT" "$DETAIL"
                rc=1 ;;
        esac
        assert_sources_untouched "during mutation '$m/$label'"
    done

    # CONFIRMED requires a POSITIVE count, not merely the absence of a
    # complaint. "Nothing reported a problem" is the verdict an empty run gives.
    if [ "$caught" -ne "$expected" ]; then
        echo "  TEETH MISSING: $caught of $expected conditions were caught."
        rc=1
    else
        echo "  TEETH CONFIRMED: all $expected conditions have a row that catches removal."
    fi
    echo
    return "$rc"
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
rc=0
case "${1:-}" in
    --self-test)
        self_test || rc=1
        ;;
    "")
        for m in "${ALL_MODULES[@]}"; do run_module "$m" || rc=1; done
        ;;
    param|block|local)
        run_module "$1" || rc=1
        ;;
    *)
        die "unknown argument '$1'; expected --self-test, or one of: ${ALL_MODULES[*]}"
        ;;
esac

assert_sources_untouched "by the run as a whole"

# Each mutant object carries a mutation. Leaving one on disk would hand the next
# `make <pass>_escape_teeth_test` a stale object for a source it no longer
# names, so remove every object and binary this script built.
for m in "${ALL_MODULES[@]}"; do
    rm -f "build/teeth/${m}_escape.o" "./${m}_escape_teeth_test"
done

if [ "$rc" -eq 0 ] && [ "$WORK_IS_OURS" = yes ]; then
    rm -rf -- "$WORK"
else
    echo "mutants and build/run logs kept in $WORK"
fi
exit "$rc"
