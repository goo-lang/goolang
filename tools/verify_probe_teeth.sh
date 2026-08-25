#!/usr/bin/env bash
# Prove the probe macros can report a failure.
#
# One macro generates roughly 150 tests in phase 4. If it cannot fail, all 150
# pass while asserting nothing and nothing downstream notices. This is the
# check that stands between that and a merged PR.
#
# Each defect is paired with a CONTROL over the same program. A defect that
# fails in both cases proves the program is broken, not that the macro works --
# the same reasoning as orca's tools/verify_sanitizers.sh.
#
# bazel test exit codes: 0 = passed, 3 = built fine but the test failed. Any
# other non-zero is a tool failure, never a test verdict. The status is read
# directly off bazel, never through a pipe.
#
# Exit: 0 the macros have teeth, 1 a macro lost them, 2 bazel itself failed.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
BAZEL="${BAZEL:-bazel}"

status() {
    "$BAZEL" test "$1" --test_output=summary --nocache_test_results >/dev/null 2>&1
    return $?
}

rc_overall=0
check() {  # label, target, want: "pass" or "fail"
    local label="$1" target="$2" want="$3" got
    status "$target"; got=$?
    if [ "$got" -ne 0 ] && [ "$got" -ne 3 ]; then
        printf '%-24s TOOL FAILURE  bazel exit %d\n' "$label" "$got"
        rc_overall=2; return
    fi
    if [ "$want" = "pass" ] && [ "$got" -eq 0 ]; then
        printf '%-24s ok            passes as declared\n' "$label"; return
    fi
    if [ "$want" = "fail" ] && [ "$got" -eq 3 ]; then
        printf '%-24s HAS TEETH     red as declared\n' "$label"; return
    fi
    if [ "$want" = "pass" ]; then
        printf '%-24s BROKEN        control failed, so the defects prove nothing\n' "$label"
    else
        printf '%-24s NO TEETH      defect PASSED\n' "$label"
    fi
    [ "$rc_overall" -ne 2 ] && rc_overall=1
}

check "output control"  //testing/teeth:probe_control        pass
check "output defect"   //testing/teeth:probe_output_defect  fail
check "exit control"    //testing/teeth:probe_exit_control   pass
check "exit defect"     //testing/teeth:probe_exit_defect    fail
check "stderr defect"   //testing/teeth:probe_stderr_defect  fail
check "reject control"  //testing/teeth:probe_reject_control pass
check "reject defect"   //testing/teeth:probe_reject_defect  fail

if [ "$rc_overall" -eq 0 ]; then
    echo "verify_probe_teeth: PASS both macros report failures and their controls pass"
fi
exit "$rc_overall"
