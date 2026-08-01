#!/usr/bin/env bash
# Which row tables actually cover which arm of the shared escape engine?
#
# src/types/escape_core.c is one taint-propagation walk feeding three passes
# (param_escape, block_escape, local_escape). PR #255 extracted it. During that
# work ONE arm was mutation-tested by hand -- AST_POSTFIX_EXPR -- and only
# local_escape's table noticed. The other 54 rows stayed green, so they prove
# nothing about `i++`.
#
# That result was an accident of which arm happened to get tried. This script
# tries all of them, so the answer is measured instead of anecdotal.
#
# THE MUTATION. escape_expr_taint's default arm does not merely return
# all-taint, it calls escape_mark() on the spot. So an ABSENT arm marks every
# slot escaping the instant that construct appears. This script reproduces an
# absent arm by injecting a guard at the top of the function that sends one
# node type straight to that same behaviour. Injecting is deliberate: the 17
# arms have three different shapes (one-line return, braced block, fallthrough
# label), and deleting a `case` label with sed would leave unreachable code or
# fall through into the previous arm's body -- a DIFFERENT mutation than the one
# intended.
#
# DIRECTION OF THE MUTATION. A missing arm makes the pass MORE conservative,
# never less. A mutation here therefore cannot make the shipped compiler
# dangerous. It can only make this matrix wrong.
#
# THE FAILURE MODE THIS SCRIPT GUARDS. A mutation that does not COMPILE looks
# exactly like a covered arm: the suite did not pass. So does a crash. Every
# verdict below is read from the suite's own `summary:` line, never from an
# exit status alone, and a run with no summary line is INCONCLUSIVE rather than
# COVERED. Run --self-test first; it proves this script can report both a
# positive and a negative before any cell of the matrix is believed.
#
# Usage:
#   scripts/escape_arm_coverage.sh --self-test     # the three controls
#   scripts/escape_arm_coverage.sh                 # full matrix, all arms
#   scripts/escape_arm_coverage.sh AST_INDEX_EXPR  # one arm
#   scripts/escape_arm_coverage.sh --baseline      # unmutated suites only

set -uo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "FATAL: not in a git repository" >&2; exit 2; }
cd "$REPO_ROOT" || exit 2

ENGINE="src/types/escape_core.c"
ANCHOR='    if (!expr) return escape_taint_new(n);'
# The statement walk is a SECOND arm population, and the more dangerous one:
# escape_walk_stmt's arms ARE the sinks (`return`, `go f(x)`, assignment,
# channel send), so skipping one stops a value being marked at all. An
# expression arm only propagates taint; a statement arm decides what escapes.
STMT_ANCHOR='    for (; stmt; stmt = stmt->next) {'
MARKER='ESCAPE_ARM_COVERAGE_MUTATION'
SUITES=(param block local)
WORK="${ESCAPE_ARM_WORKDIR:-$(mktemp -d)}"

die() { printf 'FATAL: %s\n' "$*" >&2; exit 2; }

# ---------------------------------------------------------------------------
# The engine is restored on EVERY exit path, including a kill. Without this a
# mutated escape_core.c could be committed by the next `git add`.
# ---------------------------------------------------------------------------
# ONLY restore if this script actually injected something.
#
# This guard exists because the trap DESTROYED uncommitted work. `main` runs
# assert_engine_clean first, and when that found a modified escape_core.c it
# called `die` -- which fired the EXIT trap, which ran `git checkout --` and threw
# away the very edit the guard was complaining about. The guard was meant to
# protect that work and deleted it instead. Measured: an uncommitted `is_append`
# arm vanished, and the test row that covered it went from PASS to FAIL.
INJECTED=0
restore_engine() {
    [ "$INJECTED" -eq 1 ] || return 0
    git checkout -- "$ENGINE" 2>/dev/null
    INJECTED=0
}
trap restore_engine EXIT INT TERM

assert_engine_clean() {
    git diff --quiet -- "$ENGINE" \
        || die "$ENGINE is modified; commit or stash before running"
}

# ---------------------------------------------------------------------------
# The arm list is READ FROM THE SOURCE, not hardcoded. A new arm added to the
# engine joins the matrix automatically instead of being silently uncovered.
# ---------------------------------------------------------------------------
list_arms() {
    awk '/^TaintSet escape_expr_taint\(/,/^}$/' "$ENGINE" \
        | grep -oP '^\s*case \KAST_[A-Z_0-9]+(?=:)'
}

list_stmt_arms() {
    awk '/^void escape_walk_stmt\(/,/^}$/' "$ENGINE" \
        | grep -oP '^\s*case \KAST_[A-Z_0-9]+(?=:)'
}

# Which arm population does this mode mutate?
mode_is_stmt() { case "$1" in stmt-*) return 0 ;; *) return 1 ;; esac; }

arms_for_mode() {
    if mode_is_stmt "$1"; then list_stmt_arms; else list_arms; fi
}

# ---------------------------------------------------------------------------
# Two mutation directions, because they test two DIFFERENT row populations and
# only one of them is the safety-critical one.
#
#   MODE=over   the arm behaves as if ABSENT: all-taint, marked on the spot.
#               This is what a deleted `case` label does. It makes the pass
#               MORE conservative, so ONLY a PRECISION row (expects false) can
#               detect it. Answers: "would we notice losing reclamation?"
#
#   MODE=under  the arm claims the expression aliases NOTHING. This is
#               UNDER-marking, which escape_core.h names as "the ONLY bug class
#               that can dangle a pointer". It makes the pass LESS conservative,
#               so ONLY a SOUNDNESS row (expects true) can detect it. Answers:
#               "would we notice a use-after-free?"
#
# A precision row cannot catch an `under` mutation and a soundness row cannot
# catch an `over` one, so the two matrices are complementary, not redundant.
# ---------------------------------------------------------------------------
#   MODE=stmt-over   the statement arm behaves as ABSENT: escape_mark_all, which
#                    is exactly what walk_stmt's default arm does. Precision.
#   MODE=stmt-under  the statement is SKIPPED ENTIRELY, which DELETES A SINK.
#                    A skipped `return` never marks the returned local, and T4
#                    would then free a pointer the caller still holds. This is
#                    the highest-consequence mutation the harness can make.
inject() {
    local arm="$1" mode="${2:-over}" anchor="$ANCHOR" n
    mode_is_stmt "$mode" && anchor="$STMT_ANCHOR"
    n=$(grep -cF -- "$anchor" "$ENGINE")
    [ "$n" -eq 1 ] || die "anchor matched $n times in $ENGINE, expected exactly 1"

    python3 - "$ENGINE" "$anchor" "$arm" "$MARKER" "$mode" <<'PY'
import sys
path, anchor, arm, marker, mode = sys.argv[1:6]
src = open(path).read()
if src.count(anchor) != 1:
    sys.exit("anchor count changed under us")
if mode == "over":
    guard = (
        "    if (expr && (int)expr->type == (int)" + arm + ") {\n"
        "        TaintSet t_mut = escape_taint_all(n);\n"
        "        escape_mark(ctx, &t_mut, ESCAPE_REASON_UNCLASSIFIED);\n"
        "        return t_mut;\n"
        "    }\n")
elif mode == "under":
    guard = (
        "    if (expr && (int)expr->type == (int)" + arm + ") {\n"
        "        return escape_taint_new(n);\n"
        "    }\n")
elif mode == "stmt-over":
    # `continue` runs the for-loop's `stmt = stmt->next`, so this is exactly
    # "this statement kind falls to the default arm".
    guard = (
        "        if (stmt && (int)stmt->type == (int)" + arm + ") {\n"
        "            escape_mark_all(ctx, ESCAPE_REASON_UNCLASSIFIED);\n"
        "            continue;\n"
        "        }\n")
elif mode == "stmt-under":
    guard = (
        "        if (stmt && (int)stmt->type == (int)" + arm + ") { continue; }\n")
else:
    sys.exit("unknown mutation mode: " + mode)
block = anchor + "\n    /* " + marker + " " + mode + " */\n" + guard
open(path, "w").write(src.replace(anchor, block, 1))
PY
    [ $? -eq 0 ] || die "injection script failed for $arm ($mode)"
    grep -q "$MARKER" "$ENGINE" || die "injection for $arm ($mode) did not land in $ENGINE"
    INJECTED=1
}

build() {
    make param_escape_test block_escape_test local_escape_test > "$1" 2>&1
    return $?
}

# ---------------------------------------------------------------------------
# --reach: how many times does each arm actually RUN during each suite?
#
# The mutation matrix alone cannot tell two very different causes apart. An arm
# reads GAP either because NO fixture contains that construct, or because the
# fixtures that contain it are all SOUNDNESS rows (expect true), which a
# conservative mutation cannot flip. The first needs a new fixture; the second
# needs a precision row on an existing one. This mode separates them.
#
# The probe does NOT return, so behaviour is unchanged and one build serves all
# 17 arms.
# ---------------------------------------------------------------------------
inject_reach() {
    local which="$1"; shift
    local arms=("$@") body="" arm n anchor="$ANCHOR" var="expr"
    if [ "$which" = "stmt" ]; then anchor="$STMT_ANCHOR"; var="stmt"; fi
    n=$(grep -cF -- "$anchor" "$ENGINE")
    [ "$n" -eq 1 ] || die "anchor matched $n times in $ENGINE, expected exactly 1"
    for arm in "${arms[@]}"; do
        body+="        case ${arm}: fputs(\"ARMHIT ${arm}\\n\", stderr); break;"$'\n'
    done
    python3 - "$ENGINE" "$anchor" "$MARKER" "$body" "$var" <<'PY'
import sys
path, anchor, marker, body, var = sys.argv[1:6]
src = open(path).read()
if src.count(anchor) != 1:
    sys.exit("anchor count changed under us")
guard = (
    anchor + "\n"
    "    /* " + marker + " */\n"
    "    if (" + var + ") { switch (" + var + "->type) {\n"
    + body +
    "        default: break;\n"
    "    } }\n"
)
open(path, "w").write(src.replace(anchor, guard, 1))
PY
    [ $? -eq 0 ] || die "reach injection failed"
    grep -q "$MARKER" "$ENGINE" || die "reach injection did not land in $ENGINE"
    INJECTED=1
}

reach() {
    local which="$1"; shift
    local arms=("$@") s arm
    inject_reach "$which" "${arms[@]}"
    build "$WORK/build_reach.log" || die "reach build failed; see $WORK/build_reach.log"
    for s in "${SUITES[@]}"; do
        "./${s}_escape_test" > "$WORK/reach_${s}.out" 2> "$WORK/reach_${s}.err"
        grep -q "${s}_escape_test summary:" "$WORK/reach_${s}.out" \
            || die "$s produced no summary line under the reach probe"
    done
    restore_engine

    printf '| Arm | param hits | block hits | local hits |\n|---|---|---|---|\n'
    for arm in "${arms[@]}"; do
        printf '| `%s` | %s | %s | %s |\n' "$arm" \
            "$(grep -c "ARMHIT ${arm}\$" "$WORK/reach_param.err")" \
            "$(grep -c "ARMHIT ${arm}\$" "$WORK/reach_block.err")" \
            "$(grep -c "ARMHIT ${arm}\$" "$WORK/reach_local.err")"
    done
}

# Verdict for one suite, read from its own summary line.
#   PASS  - ran, summary present, zero assertion failures
#   FAIL  - ran, summary present, at least one assertion failure
#   CRASH - binary produced no summary line (signal, abort, early exit)
#   PARSE - summary line present but not in the expected shape
suite_status() {
    local s="$1" log="$2" failed
    "./${s}_escape_test" > "$log" 2>&1
    if ! grep -q "${s}_escape_test summary:" "$log"; then echo CRASH; return; fi
    failed=$(grep -oP "${s}_escape_test summary: [0-9]+ assertions passed, \K[0-9]+" "$log")
    [ -n "$failed" ] || { echo PARSE; return; }
    [ "$failed" -gt 0 ] && echo FAIL || echo PASS
}

status_to_verdict() {
    case "$1" in
        FAIL)  echo "COVERED" ;;
        PASS)  echo "GAP" ;;
        CRASH) echo "INCONCLUSIVE-crash" ;;
        *)     echo "INCONCLUSIVE-parse" ;;
    esac
}

# Echoes "param_status block_status local_status", or "BUILD" if it did not compile.
measure_arm() {
    local arm="$1" mode="${2:-over}" out=() s st
    if [ "$arm" != "__baseline__" ]; then inject "$arm" "$mode"; fi
    if ! build "$WORK/build_${mode}_${arm}.log"; then restore_engine; echo "BUILD"; return; fi
    for s in "${SUITES[@]}"; do
        st=$(suite_status "$s" "$WORK/${mode}_${arm}_${s}.log")
        out+=("$st")
    done
    restore_engine
    echo "${out[*]}"
}

# ---------------------------------------------------------------------------
# --self-test: prove the instrument can report BOTH answers on demand.
# ---------------------------------------------------------------------------
self_test() {
    local rc=0 r
    echo "=== Control 1: baseline, no mutation -> all three PASS ==="
    r=$(measure_arm __baseline__)
    echo "  got: $r"
    [ "$r" = "PASS PASS PASS" ] || { echo "  CONTROL 1 FAILED"; rc=1; }

    # This control READ "PASS PASS FAIL" until param row 24 and block row 32
    # landed, because that was PR #255's measured state and the ledger item.
    # Closing the gap changed the fact the control asserted, which is the point
    # of the change. It now doubles as a regression guard on those two rows:
    # delete either and this goes back to a PASS.
    echo "=== Control 2: over/AST_POSTFIX_EXPR -> all three FAIL (param row 24, block row 32) ==="
    r=$(measure_arm AST_POSTFIX_EXPR over)
    echo "  got: $r"
    [ "$r" = "FAIL FAIL FAIL" ] || { echo "  CONTROL 2 FAILED"; rc=1; }

    # A MIXED pattern, and the strongest control here. Controls 2-4 all expect
    # every suite to fail, and a bluntly broken build produces that too. This one
    # expects param to PASS while the others fail, so a guard that is injected
    # wrongly -- or a build that ignores the injection -- cannot produce it by
    # accident.
    #
    # UPDATED 2026-08-01, from "PASS FAIL PASS", and NOT because anything got
    # more conservative. local_escape_test's ADR 0005 rows assert the whole
    # reason SET with ==, and that catches over-marking a boolean row cannot:
    #
    #   FAIL: local 'p' reasons=UNCLASSIFIED|CALLEE_VALUE, expected CALLEE_VALUE
    #
    # An `over` mutation adds UNCLASSIFIED to every slot it touches. A row
    # asserting `escapes == true` cannot see that, because true stays true --
    # which is why this whole direction needed a PRECISION row before. A row
    # asserting the exact set sees the extra bit while still being a soundness
    # row. Local row 42 is that row, and it closed four arms at once:
    # AST_UNARY_EXPR, AST_SELECTOR_EXPR, AST_FUNC_LIT and AST_STRUCT_LITERAL
    # all went GAP -> COVERED for local in the same commit.
    echo "=== Control 2b: over/AST_UNARY_EXPR -> param PASS, block+local FAIL (mixed) ==="
    r=$(measure_arm AST_UNARY_EXPR over)
    echo "  got: $r"
    [ "$r" = "PASS FAIL FAIL" ] || { echo "  CONTROL 2b FAILED"; rc=1; }

    echo "=== Control 3: over/AST_IDENTIFIER -> all three FAIL ==="
    r=$(measure_arm AST_IDENTIFIER over)
    echo "  got: $r"
    [ "$r" = "FAIL FAIL FAIL" ] || { echo "  CONTROL 3 FAILED"; rc=1; }

    # The `under` direction needs its own controls. It flips a DIFFERENT row
    # population (soundness, not precision), so controls 1-3 say nothing about
    # whether the harness can drive it.
    echo "=== Control 4: under/AST_IDENTIFIER -> all three FAIL ==="
    r=$(measure_arm AST_IDENTIFIER under)
    echo "  got: $r"
    [ "$r" = "FAIL FAIL FAIL" ] || { echo "  CONTROL 4 FAILED"; rc=1; }

    # A TRUE negative, and the only one this harness can assert independently:
    # --reach measures 0 hits for AST_TYPE_ASSERT in all three suites, so no
    # mutation of it can change any verdict. If this reads anything but all
    # PASS, the injection is reaching code it should not.
    echo "=== Control 5: under/AST_TYPE_ASSERT -> all three PASS (0 hits, cross-checked by --reach) ==="
    r=$(measure_arm AST_TYPE_ASSERT under)
    echo "  got: $r"
    [ "$r" = "PASS PASS PASS" ] || { echo "  CONTROL 5 FAILED"; rc=1; }

    # The statement walk is a different anchor, a different injection shape and a
    # different arm list, so controls 1-5 prove nothing about it.
    echo "=== Control 6: stmt-under/AST_BLOCK_STMT -> all three FAIL (nothing is walked) ==="
    r=$(measure_arm AST_BLOCK_STMT stmt-under)
    echo "  got: $r"
    [ "$r" = "FAIL FAIL FAIL" ] || { echo "  CONTROL 6 FAILED"; rc=1; }

    # MIXED pattern for the statement direction, and the one that proves the
    # injection lands where it should rather than everywhere.
    #
    # AST_ARENA_BLOCK was the obvious candidate and it is WRONG: block_escape
    # runs two passes, and Pass 2 drives escape_walk_stmt from the arena block's
    # BODY, so this arm only ever sees a NESTED arena block. No fixture has one,
    # so skipping it moves nothing. Checked, not assumed.
    # UPDATED 2026-08-01, from "PASS FAIL PASS", and for the same cause as
    # control 2 above: a suite got better and the control did not follow.
    #
    # local_escape sets defer_is_like_go = TRUE, so `defer sink(x)` is the only
    # thing that marks `x`. Skipping the statement deletes that sink, `x` reads
    # non-escaping, and local row 34 -- "local passed to a DEFER whose callee
    # does not retain it -> true" -- FAILS. That row is doing exactly its job:
    # its own comment says a release emitted before the deferred call runs
    # would dangle.
    #
    # Row 34 landed on 2026-08-01 (d60989d, the escape teeth work). This
    # control was written on 2026-07-29 (b490b6d), when local had no row that
    # noticed, so PASS was the measured truth THEN and is stale now.
    #
    # PARAM STAYS PASS, AND THAT IS A REAL GAP RATHER THAN A CORRECT ANSWER.
    # param_escape treats a defer as an ordinary call, so skipping the
    # statement drops its argument from the retention sink too -- an
    # under-mark. No param row has a defer whose callee retains its argument,
    # so nothing notices. A row for that shape would flip this cell to FAIL,
    # and this comment is the record of what is missing.
    echo "=== Control 7: stmt-under/AST_DEFER_STMT -> param PASS, block+local FAIL (mixed) ==="
    r=$(measure_arm AST_DEFER_STMT stmt-under)
    echo "  got: $r"
    [ "$r" = "PASS FAIL FAIL" ] || { echo "  CONTROL 7 FAILED"; rc=1; }

    assert_engine_clean
    if [ "$rc" -eq 0 ]; then
        echo "SELF-TEST PASSED: the harness reports both a positive and a negative."
    else
        echo "SELF-TEST FAILED: do not believe any cell of the matrix."
    fi
    return "$rc"
}

# ---------------------------------------------------------------------------
# EQUIVALENT MUTANTS. A mutation that is behaviourally identical to the code it
# replaces can never be detected, so reporting GAP for it would be a FALSE
# finding -- it claims a missing row where no row could ever help.
#
# `under` replaces an arm with `return escape_taint_new(n);`. Any arm whose body
# already IS that line is an equivalent mutant. AST_LITERAL is one: a literal
# aliases nothing, which is the same answer the mutation gives.
#
# This is computed from the source, not hardcoded, so an arm that later becomes
# (or stops being) equivalent is reclassified automatically.
# ---------------------------------------------------------------------------
# Body of one `case` arm, with the leading indentation stripped so the same
# extractor serves the expression walk (8 spaces) and the statement walk (12).
#
# MUST tolerate a brace on the label line. Most statement arms are written
# `case AST_RETURN_STMT: {`, and an extractor that only matched a bare
# `case AST_X:` returned an EMPTY body for every one of them. That was harmless
# while the only test was "does the body equal `return escape_taint_new(n);`"
# (an empty body just fails it), and it produced 15 FALSE "no-op" verdicts the
# moment a test asked "is the body empty?". An empty extraction must never be
# mistaken for a meaningful answer, so `arm_body_or_die` refuses to return one.
arm_body() {
    local arm="$1" which="${2:-expr}" range
    if [ "$which" = "stmt" ]; then
        range='/^void escape_walk_stmt\(/,/^}$/'
    else
        range='/^TaintSet escape_expr_taint\(/,/^}$/'
    fi
    awk "$range" "$ENGINE" | awk -v a="case $arm:" '
        { line = $0; sub(/^[ \t]+/, "", line) }
        !f && index(line, a) == 1 {
            f = 1
            rest = substr(line, length(a) + 1)
            sub(/^[ \t]+/, "", rest)
            if (rest != "") print rest
            next
        }
        f && (line ~ /^case AST_/ || line ~ /^default:/) { exit }
        f { print line }'
}

# True when the arm is a genuine no-op: nothing but braces, a `break;`, or a
# fallthrough to the next label. Braces are stripped, so `{ break; }` counts.
arm_is_noop() {
    local body
    body=$(arm_body "$1" "$2" | tr -d ' \n\t{}')
    [ -z "$body" ] || [ "$body" = "break;" ]
}

# The arm label must EXIST. A typo, a renamed arm, or a changed brace style
# would otherwise silently classify every arm as a no-op.
assert_arm_found() {
    local arm="$1" which="$2" range
    if [ "$which" = "stmt" ]; then
        range='/^void escape_walk_stmt\(/,/^}$/'
    else
        range='/^TaintSet escape_expr_taint\(/,/^}$/'
    fi
    awk "$range" "$ENGINE" | grep -qE "^[[:space:]]+case ${arm}:" \
        || die "arm $arm not found in the $which walk; the extractor is stale"
}

is_equivalent_mutant() {
    local arm="$1" mode="$2" body
    case "$mode" in
        under)
            # The `under` guard IS `return escape_taint_new(n);`. An arm whose
            # body already is that line cannot be detected by any row.
            assert_arm_found "$arm" expr
            body=$(arm_body "$arm" expr | tr -d ' \n\t')
            [ "$body" = "returnescape_taint_new(n);" ]
            ;;
        stmt-under)
            # `stmt-under` SKIPS the statement. An arm that already does nothing
            # cannot be made to do less, so GAP there would be a false finding.
            # AST_BREAK_STMT and AST_CONTINUE_STMT are the pair: they share a
            # body that is a bare `break;`.
            assert_arm_found "$arm" stmt
            arm_is_noop "$arm" stmt
            ;;
        *) return 1 ;;
    esac
}

# Prints the no-op classification for every statement arm, so the equivalence
# rule can be checked by eye instead of trusted. `--classify` exists because an
# earlier extractor called 15 of 20 arms no-ops and the matrix looked plausible.
classify_stmt_arms() {
    local arm
    printf '| Arm | no-op? | body (normalised, first 60 chars) |\n|---|---|---|\n'
    while read -r arm; do
        assert_arm_found "$arm" stmt
        printf '| `%s` | %s | `%.60s` |\n' "$arm" \
            "$(arm_is_noop "$arm" stmt && echo YES || echo no)" \
            "$(arm_body "$arm" stmt | tr -d '\n' | tr -s ' ')"
    done < <(list_stmt_arms)
}

matrix() {
    local mode="$1"; shift
    local arms=("$@") arm r p b l
    printf '| Arm | param (23) | block (31) | local (16) |\n|---|---|---|---|\n'
    for arm in "${arms[@]}"; do
        if is_equivalent_mutant "$arm" "$mode"; then
            printf '| `%s` | N/A-equivalent | N/A-equivalent | N/A-equivalent |\n' "$arm"
            continue
        fi
        r=$(measure_arm "$arm" "$mode")
        if [ "$r" = "BUILD" ]; then
            printf '| `%s` | INCONCLUSIVE-build | INCONCLUSIVE-build | INCONCLUSIVE-build |\n' "$arm"
            continue
        fi
        read -r p b l <<<"$r"
        printf '| `%s` | %s | %s | %s |\n' "$arm" \
            "$(status_to_verdict "$p")" \
            "$(status_to_verdict "$b")" \
            "$(status_to_verdict "$l")"
    done
}

run_matrix() {
    local mode="$1" arms
    mapfile -t arms < <(arms_for_mode "$mode")
    [ "${#arms[@]}" -gt 0 ] || die "no arms found in $ENGINE for mode $mode"
    echo "arms found: ${#arms[@]} (mode: $mode)" >&2
    matrix "$mode" "${arms[@]}"
}

main() {
    assert_engine_clean
    echo "workdir: $WORK" >&2
    case "${1:-}" in
        --self-test) self_test ;;
        --baseline)  echo "baseline: $(measure_arm __baseline__)" ;;
        --list)      list_arms ;;
        --reach)     mapfile -t arms < <(list_arms)
                     [ "${#arms[@]}" -gt 0 ] || die "no arms found in $ENGINE"
                     reach expr "${arms[@]}" ;;
        --reach-stmt) mapfile -t arms < <(list_stmt_arms)
                     [ "${#arms[@]}" -gt 0 ] || die "no statement arms found in $ENGINE"
                     reach stmt "${arms[@]}" ;;
        --over|"")     run_matrix over ;;
        --under)       run_matrix under ;;
        --stmt-over)   run_matrix stmt-over ;;
        --stmt-under)  run_matrix stmt-under ;;
        --list-stmt)   list_stmt_arms ;;
        --classify)    classify_stmt_arms ;;
        *)             matrix "${2:-over}" "$1" ;;
    esac
}

main "$@"
