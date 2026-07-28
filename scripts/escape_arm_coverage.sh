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
MARKER='ESCAPE_ARM_COVERAGE_MUTATION'
SUITES=(param block local)
WORK="${ESCAPE_ARM_WORKDIR:-$(mktemp -d)}"

die() { printf 'FATAL: %s\n' "$*" >&2; exit 2; }

# ---------------------------------------------------------------------------
# The engine is restored on EVERY exit path, including a kill. Without this a
# mutated escape_core.c could be committed by the next `git add`.
# ---------------------------------------------------------------------------
restore_engine() { git checkout -- "$ENGINE" 2>/dev/null; }
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
inject() {
    local arm="$1" mode="${2:-over}" n
    n=$(grep -cF -- "$ANCHOR" "$ENGINE")
    [ "$n" -eq 1 ] || die "anchor matched $n times in $ENGINE, expected exactly 1"

    python3 - "$ENGINE" "$ANCHOR" "$arm" "$MARKER" "$mode" <<'PY'
import sys
path, anchor, arm, marker, mode = sys.argv[1:6]
src = open(path).read()
if src.count(anchor) != 1:
    sys.exit("anchor count changed under us")
if mode == "over":
    body = ("        TaintSet t_mut = escape_taint_all(n);\n"
            "        escape_mark(ctx, &t_mut);\n"
            "        return t_mut;\n")
elif mode == "under":
    body = "        return escape_taint_new(n);\n"
else:
    sys.exit("unknown mutation mode: " + mode)
guard = (
    anchor + "\n"
    "    /* " + marker + " " + mode + " */\n"
    "    if (expr && (int)expr->type == (int)" + arm + ") {\n"
    + body +
    "    }\n"
)
open(path, "w").write(src.replace(anchor, guard, 1))
PY
    [ $? -eq 0 ] || die "injection script failed for $arm ($mode)"
    grep -q "$MARKER" "$ENGINE" || die "injection for $arm ($mode) did not land in $ENGINE"
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
    local arms=("$@") body="" arm n
    n=$(grep -cF -- "$ANCHOR" "$ENGINE")
    [ "$n" -eq 1 ] || die "anchor matched $n times in $ENGINE, expected exactly 1"
    for arm in "${arms[@]}"; do
        body+="        case ${arm}: fputs(\"ARMHIT ${arm}\\n\", stderr); break;"$'\n'
    done
    python3 - "$ENGINE" "$ANCHOR" "$MARKER" "$body" <<'PY'
import sys
path, anchor, marker, body = sys.argv[1:5]
src = open(path).read()
if src.count(anchor) != 1:
    sys.exit("anchor count changed under us")
guard = (
    anchor + "\n"
    "    /* " + marker + " */\n"
    "    if (expr) { switch (expr->type) {\n"
    + body +
    "        default: break;\n"
    "    } }\n"
)
open(path, "w").write(src.replace(anchor, guard, 1))
PY
    [ $? -eq 0 ] || die "reach injection failed"
    grep -q "$MARKER" "$ENGINE" || die "reach injection did not land in $ENGINE"
}

reach() {
    local arms=("$@") s arm
    inject_reach "${arms[@]}"
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
    # expects the mutation to reach block and NOT param or local, so a guard that
    # is injected wrongly -- or a build that ignores the injection -- cannot
    # produce it by accident.
    echo "=== Control 2b: over/AST_UNARY_EXPR -> block FAIL only (mixed pattern) ==="
    r=$(measure_arm AST_UNARY_EXPR over)
    echo "  got: $r"
    [ "$r" = "PASS FAIL PASS" ] || { echo "  CONTROL 2b FAILED"; rc=1; }

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
arm_body() {
    awk '/^TaintSet escape_expr_taint\(/,/^}$/' "$ENGINE" \
        | awk -v a="        case $1:" '
            $0 == a { f = 1; next }
            f && /^        (case AST_|default:)/ { exit }
            f { print }'
}

is_equivalent_mutant() {
    local arm="$1" mode="$2" body
    [ "$mode" = "under" ] || return 1
    body=$(arm_body "$arm" | tr -d ' \n\t')
    [ "$body" = "returnescape_taint_new(n);" ]
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

main() {
    assert_engine_clean
    echo "workdir: $WORK" >&2
    case "${1:-}" in
        --self-test) self_test ;;
        --baseline)  echo "baseline: $(measure_arm __baseline__)" ;;
        --list)      list_arms ;;
        --reach)     mapfile -t arms < <(list_arms)
                     [ "${#arms[@]}" -gt 0 ] || die "no arms found in $ENGINE"
                     reach "${arms[@]}" ;;
        --over|"")   mapfile -t arms < <(list_arms)
                     [ "${#arms[@]}" -gt 0 ] || die "no arms found in $ENGINE"
                     echo "arms found: ${#arms[@]} (mode: over / precision)" >&2
                     matrix over "${arms[@]}" ;;
        --under)     mapfile -t arms < <(list_arms)
                     [ "${#arms[@]}" -gt 0 ] || die "no arms found in $ENGINE"
                     echo "arms found: ${#arms[@]} (mode: under / soundness)" >&2
                     matrix under "${arms[@]}" ;;
        *)           matrix "${2:-over}" "$1" ;;
    esac
}

main "$@"
