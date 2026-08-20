#!/usr/bin/env bash
# goo-check-probe — the contract of tests/unit/goo_check.h, the shared
# assertion header the C unit suites report through.
#
# Six suites used to hand-roll this: four separate copies of check(), two
# counter conventions (g_pass/g_fail vs checks/failures), two indent widths,
# and one suite (arena_routing_test.c) that printed a PASS line per check
# while the other five stayed quiet. A reader could not tell one suite's
# verdict from another's, and neither could a script.
#
# The header is now the single place that decides an exit status, so it is
# also the single place a wrong exit status can hide. Three outcomes:
#
#   PASS   0  every check passed and every declared row ran
#   FAIL   1  a check failed
#   BROKEN 2  fewer rows ran than the suite declared
#
# BROKEN is the one that earns its keep. A table whose rows stop executing --
# a grammar change the fixtures no longer parse, an early `continue` -- prints
# nothing and exits 0, which reads exactly like success. ast_free_leak_test.c
# already hand-rolled this check for that reason; goo_check_expect() makes it
# available to every suite.
set -u

PROBE="goo-check-probe"
ROOT="${GOO_CHECK_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
HDR_DIR="${GOO_CHECK_HDR_DIR:-$ROOT/tests/unit}"
# The Makefile passes CC_PROBE="$(CC)", which is "$(CCACHE) gcc" locally: TWO
# WORDS. It is left unquoted at the call site below so the shell splits it,
# exactly as scripts/goo_assert_probe.sh documents. Quoting it made every
# fixture fail to compile with "/usr/bin/ccache gcc: No such file or
# directory", and the probe read that as nine contract violations in the
# header -- a green-looking run of the WRONG experiment is the failure mode
# this whole file exists to prevent, so it is recorded here rather than
# quietly fixed.
CC_PROBE="${CC_PROBE:-cc}"
CSTD_PROBE="${CSTD_PROBE:--std=c23}"
fails=0
bad() { echo "  FAIL: $*"; fails=$((fails + 1)); }

# ---------------------------------------------------------------------------
# --self-test: the probe that demands a negative control must have one. Three
# mutations of the header plus a CONTROL -- without the control, a temp tree
# that fails to compile at all turns every row red and reads as success.
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--self-test" ]; then
	SELF="$PROBE --self-test"
	W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
	mk() { rm -rf "$W/h"; mkdir -p "$W/h"; cp "$HDR_DIR/goo_check.h" "$W/h/goo_check.h"; }
	run() { GOO_CHECK_HDR_DIR="$W/h" CC_PROBE="$CC_PROBE" CSTD_PROBE="$CSTD_PROBE" \
	        "$0" >"$W/out.log" 2>&1; }
	bad_self=0
	mutations=0

	mk
	if run; then
		echo "    ok: control (unmutated header) is GREEN"
	else
		echo "$SELF: FAIL (control already red -- the harness is broken, not the header)"
		sed 's/^/        /' "$W/out.log"; exit 1
	fi

	mutate() { # name, sed program
		mutations=$((mutations + 1))
		mk
		sed -i "$2" "$W/h/goo_check.h"
		if cmp -s "$HDR_DIR/goo_check.h" "$W/h/goo_check.h"; then
			echo "$SELF: FAIL (mutation '$1' changed nothing -- the sed no longer matches)"
			bad_self=1; return
		fi
		if run; then
			echo "$SELF: FAIL (stayed green after: $1)"
			sed 's/^/        /' "$W/out.log"; bad_self=1
		else
			echo "    ok: '$1' turns it red"
		fi
	}

	# A failed check that does not count is the whole point of the header.
	mutate "a failing check that increments nothing" \
		's/goo_check_failures++;/;/'
	# BROKEN collapsing into PASS is the silent-green failure mode.
	mutate "goo_check_done ignoring the declared row count" \
		's/goo_check_rows != goo_check_rows_expected/0/'
	# A suite that never declares a count must not opt itself out of BROKEN.
	mutate "an undeclared row count treated as satisfied" \
		's/goo_check_rows_expected < 0/0/'
	# goo_check_failed() gates the early-stop path in arena_routing_test.
	mutate "goo_check_failed() that never reports a failure" \
		's/return goo_check_failures > 0;/return false;/'
	# Quiet-on-success is a contract, not a preference: 213 gates share stdout.
	mutate "a PASS line printed for every check" \
		's|^    goo_check_checks++;|    goo_check_checks++; printf("    PASS: %s\\n", label);|'

	[ "$bad_self" -ne 0 ] && exit 1
	echo "$SELF: PASS (control green; all $mutations failure modes independently turn it red)"
	exit 0
fi

# ---------------------------------------------------------------------------
# The contract itself.
# ---------------------------------------------------------------------------
if [ ! -f "$HDR_DIR/goo_check.h" ]; then
	echo "$PROBE: FAIL ($HDR_DIR/goo_check.h does not exist)"
	exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# One fixture body, four shapes. rows_run and expect are what each case varies.
fixture() { # file, expect_arg, rows_to_run, failing_check(0|1)
	cat >"$1" <<EOF
#include "goo_check.h"

int main(void) {
    goo_check_expect($2);
    for (int r = 1; r <= $3; r++) {
        goo_check_row(r, "a row");
        goo_check(true, "first check of the row");
        goo_check($4 && r == 2 ? false : true, "second check of the row");
    }
    return goo_check_done("demo-suite");
}
EOF
}

build_run() { # fixture.c -> writes $WORK/out.txt, echoes exit status
	rm -f "$WORK/fixture" "$WORK/out.txt"
	# shellcheck disable=SC2086  # CC_PROBE and CSTD_PROBE are word-split on purpose
	if ! $CC_PROBE $CSTD_PROBE -Wall -Wextra -I"$HDR_DIR" \
	     "$1" -o "$WORK/fixture" 2>"$WORK/build.log"; then
		echo "BUILD_FAILED"
		return
	fi
	"$WORK/fixture" >"$WORK/out.txt" 2>&1
	echo "$?"
}

expect_case() { # label, fixture args..., want_status, want_grep...
	local label="$1" file="$WORK/case.c" want_status="$5"
	fixture "$file" "$2" "$3" "$4"
	local got
	got="$(build_run "$file")"
	if [ "$got" = "BUILD_FAILED" ]; then
		bad "$label: the fixture did not compile"
		sed 's/^/      /' "$WORK/build.log"
		return
	fi
	if [ "$got" != "$want_status" ]; then
		bad "$label: exit $got, expected $want_status"
		sed 's/^/      /' "$WORK/out.txt"
		return
	fi
	shift 5
	local pattern
	for pattern in "$@"; do
		if ! grep -qF -- "$pattern" "$WORK/out.txt"; then
			bad "$label: output does not contain '$pattern'"
			sed 's/^/      /' "$WORK/out.txt"
			return
		fi
	done
	echo "    ok: $label"
}

# 1. Everything passes. 3 rows, 2 checks each.
expect_case "all rows pass -> exit 0" 3 3 0 0 \
	"demo-suite: PASS (6 checks, 3 rows)"

# 2. Quiet on success. A per-check PASS line would drown 213 gates.
#
#    Written as an explicit three-way, because the obvious two-way form
#    (`[ status = 0 ] && grep PASS:` -> bad, else ok) reported "ok" when the
#    fixture failed to COMPILE: the status was merely not 0, so the else arm
#    fired. A row that cannot tell "quiet" from "never ran" is not a check.
fixture "$WORK/quiet.c" 3 3 0
quiet_status="$(build_run "$WORK/quiet.c")"
if [ "$quiet_status" != "0" ]; then
	bad "quiet-on-success: the fixture exited $quiet_status, expected 0"
	[ -f "$WORK/build.log" ] && sed 's/^/      /' "$WORK/build.log"
elif grep -q "PASS: " "$WORK/out.txt"; then
	bad "a passing check printed a per-check line; only the verdict may say PASS"
	sed 's/^/      /' "$WORK/out.txt"
else
	echo "    ok: a passing check prints nothing"
fi

# 3. One failing check. The label must reach stdout, and the count must be
#    '1 of 6' -- a header that reported '1 of 1' would hide how much ran.
expect_case "one failing check -> exit 1" 3 3 1 1 \
	"FAIL: second check of the row" \
	"demo-suite: FAIL (1 of 6 checks, 3 rows)"

# 4. The table stopped early. This is the case that exists to be caught.
expect_case "fewer rows than declared -> exit 2 BROKEN" 15 3 0 2 \
	"demo-suite: BROKEN" \
	"3 of 15 rows ran"

# 5. No row ran at all -- the same defect, at its limit.
expect_case "zero rows -> exit 2 BROKEN" 3 0 0 2 \
	"demo-suite: BROKEN" \
	"0 of 3 rows ran"

# 6. BROKEN outranks FAIL. If the table stopped, a failure count is not
#    trustworthy evidence about the code under test, so the instrument
#    verdict must win.
expect_case "a short table with a failing check reports BROKEN" 15 3 1 2 \
	"demo-suite: BROKEN"

# 7. A declaration the table has outgrown. The instrument still works, but the
#    number no longer describes it, and a number nobody maintains is the state
#    scripts/safety-baseline.txt reached at 139 dead entries out of 218.
expect_case "more rows than declared -> exit 2 BROKEN" 2 3 0 2 \
	"demo-suite: BROKEN" \
	"3 of 2 rows ran"

# 8. No declaration at all. Without this row a suite opts out of every check
#    above by leaving one line out, which is the easiest mistake to make and
#    the hardest to see in review.
#
#    The row description below is deliberately BLAND. An earlier version of
#    this row described the row as "a suite that never declared a count", and
#    goo_check_row() prints its description, so the grep below matched the
#    fixture's own text and stayed green under the mutation it exists to
#    catch. Assert on the VERDICT LINE, anchored to the artifact name -- the
#    same defect doc_claims_probe.sh shipped with on 2026-08-20.
cat >"$WORK/undeclared.c" <<'EOF'
#include "goo_check.h"

int main(void) {
    goo_check_row(1, "an ordinary row");
    goo_check(true, "an ordinary check");
    return goo_check_done("demo-suite");
}
EOF
got="$(build_run "$WORK/undeclared.c")"
if [ "$got" != "2" ]; then
	bad "a suite with no goo_check_expect() exited $got, expected 2 (BROKEN)"
	sed 's/^/      /' "$WORK/out.txt"
elif ! grep -qF "demo-suite: BROKEN -- the suite never declared a row count." "$WORK/out.txt"; then
	bad "the undeclared-count verdict does not name the missing declaration"
	sed 's/^/      /' "$WORK/out.txt"
else
	echo "    ok: no goo_check_expect() -> exit 2 BROKEN"
fi

# 9. goo_check_failed() is what a suite reads to stop before rows that only
#    mean something against a sound fixture. It must be false before any
#    failure and true after one.
cat >"$WORK/failed.c" <<'EOF'
#include "goo_check.h"

int main(void) {
    goo_check_expect(1);
    goo_check_row(1, "an ordinary row");
    if (goo_check_failed()) printf("  FAIL: goo_check_failed() was true before any check\n");
    goo_check(false, "a check that fails on purpose");
    if (!goo_check_failed()) printf("  FAIL: goo_check_failed() stayed false after a failure\n");
    return goo_check_done("demo-suite");
}
EOF
got="$(build_run "$WORK/failed.c")"
if [ "$got" != "1" ]; then
	bad "the goo_check_failed() fixture exited $got, expected 1"
	sed 's/^/      /' "$WORK/out.txt"
elif grep -qF "goo_check_failed()" "$WORK/out.txt"; then
	bad "goo_check_failed() does not track goo_check()"
	sed 's/^/      /' "$WORK/out.txt"
else
	echo "    ok: goo_check_failed() tracks goo_check()"
fi

if [ "$fails" -ne 0 ]; then
	echo "$PROBE: FAIL ($fails problem(s) in $HDR_DIR/goo_check.h)"
	exit 1
fi
echo "$PROBE: PASS (9 contract rows over $HDR_DIR/goo_check.h)"
exit 0
