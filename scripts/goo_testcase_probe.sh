#!/usr/bin/env bash
# goo-testcase-probe — the contract of GOO_TESTCASE, the boundary marker.
#
# WHAT A MARKER IS. `GOO_TESTCASE(x)` is an author's claim that a boundary
# matters: some test must drive `x` BOTH true and false. SQLite carries 1,184
# of them (https://sqlite.org/testing.html) and that is how it holds 100% MC/DC
# rather than merely measuring it.
#
# WHY IT CAN BE A GATE WHEN COVERAGE IS NOT. scripts/coverage_corpus.sh refuses
# to be a gate on purpose: "a coverage target invites tests that raise the
# number instead of tests that find bugs". That is true of a PERCENTAGE and
# false of a MARKER. You cannot inflate "this specific boundary was driven both
# ways" with a shallow test -- either a test reached it or no test did. So the
# marker is gated and the percentage stays a measurement.
#
# WHAT THIS PROBE DOES NOT DO. It does not run the real corpus. That takes
# minutes and is serial (gcov counters have no lock), which is why
# `coverage-goo` is not in verify-core either. This probe proves the MECHANISM
# on a fixture in seconds: inert in a production build, a real two-way branch
# under coverage, and a report that can tell one case from the other.
set -u

PROBE="goo-testcase-probe"
# A Bazel sh_test starts with $PWD already AT the runfiles root, but $0
# resolves to <runfiles>/_main/tests/probes/goo_testcase_probe.sh -- a symlink
# one directory too deep for the old dirname-based fallback, so ROOT gained a
# stray tests/ segment (measured: INC_DIR resolved to ".../tests/include" and
# the probe reported "goo_assert.h missing"). git rev-parse fails in the
# sandbox (no .git), so the fallback is $PWD itself, which IS the runfiles
# root there and the repo root here.
ROOT="${GOO_TESTCASE_ROOT:-$(git rev-parse --show-toplevel 2>/dev/null || pwd)}"
INC_DIR="${GOO_TESTCASE_INC:-$ROOT/include}"
REPORT="${GOO_TESTCASE_REPORT:-$ROOT/scripts/testcase_report.sh}"
# The Makefile passes CC_PROBE="$(CC)", which is "$(CCACHE) gcc" locally: two
# words. Left unquoted at the call sites so the shell splits it, as
# scripts/goo_assert_probe.sh and scripts/goo_check_probe.sh both document.
CC_PROBE="${CC_PROBE:-gcc}"
CSTD_PROBE="${CSTD_PROBE:--std=c23}"
fails=0
bad() { echo "  FAIL: $*"; fails=$((fails + 1)); }

# ---------------------------------------------------------------------------
# --self-test. Three mutations plus a CONTROL. Without the control a fixture
# that stopped compiling turns every row red and reads as success.
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--self-test" ]; then
	SELF="$PROBE --self-test"
	W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
	mk() {
		rm -rf "$W/t"; mkdir -p "$W/t/include" "$W/t/scripts"
		cp "$INC_DIR/goo_assert.h" "$W/t/include/goo_assert.h"
		cp "$REPORT" "$W/t/scripts/testcase_report.sh"
	}
	run() {
		GOO_TESTCASE_INC="$W/t/include" GOO_TESTCASE_REPORT="$W/t/scripts/testcase_report.sh" \
		CC_PROBE="$CC_PROBE" CSTD_PROBE="$CSTD_PROBE" "$0" >"$W/out.log" 2>&1
	}
	bad_self=0
	mutations=0

	mk
	if run; then
		echo "    ok: control (unmutated header and report) is GREEN"
	else
		echo "$SELF: FAIL (control already red -- the harness is broken, not the marker)"
		sed 's/^/        /' "$W/out.log"; exit 1
	fi

	# Each mutation must CHANGE the file. A sed that no longer matches leaves
	# the copy pristine, the probe stays green for the right reason, and the
	# self-test reads that as a missing negative control -- which is exactly
	# backwards. Compare before judging.
	mutate() {  # name, file-under-t, sed program
		mutations=$((mutations + 1))
		mk
		sed -i "$3" "$W/t/$2"
		if cmp -s "$W/t/$2" "$(orig_of "$2")"; then
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
	orig_of() {  # file-under-t -> the tracked original it was copied from
		case "$1" in
			include/goo_assert.h)        echo "$INC_DIR/goo_assert.h" ;;
			scripts/testcase_report.sh)  echo "$REPORT" ;;
			*) echo "/dev/null" ;;
		esac
	}

	# A marker that records nothing under coverage leaves no branch to read.
	mutate "a coverage marker that never records a hit" \
		"include/goo_assert.h" \
		's/goo_testcase_hit(__FILE__, __LINE__)/((void)0)/'
	# A report that clears every marker is the silent-green case.
	mutate "a report that never names an unexercised marker" \
		"scripts/testcase_report.sh" \
		's/if len(counts) < 2 or min(counts) == 0:/if False:/'
	# A report that measured nothing must say so, not print a clean zero.
	mutate "a report that treats 'no gcov output' as success" \
		"scripts/testcase_report.sh" \
		's/if \[ "$emitted" -eq 0 \]; then/if false; then/'

	[ "$bad_self" -ne 0 ] && exit 1
	echo "$SELF: PASS (control green; all $mutations failure modes independently turn it red)"
	exit 0
fi

# ---------------------------------------------------------------------------
# The contract.
# ---------------------------------------------------------------------------
[ -f "$INC_DIR/goo_assert.h" ] || { echo "$PROBE: FAIL ($INC_DIR/goo_assert.h missing)"; exit 1; }
[ -f "$REPORT" ]              || { echo "$PROBE: FAIL ($REPORT missing)"; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# One marker, on a known line. `which` picks how many ways the marker is driven
# so the same fixture serves the one-way and the both-ways rows.
write_fixture() {  # path
	cat >"$1" <<'EOF'
#include "goo_assert.h"
#include <stdlib.h>

static int classify(int n) {
    GOO_TESTCASE(n > 100);
    return n > 100 ? 1 : 0;
}

int main(int argc, char** argv) {
    (void)argv;
    /* argc == 1 drives the marker false only; argc == 2 drives it both ways. */
    classify(1);
    if (argc > 1) classify(500);
    return 0;
}
EOF
}

MARKER_LINE=5   # the GOO_TESTCASE line in the fixture above

# --- Row 1: inert in a production build -----------------------------------
write_fixture "$WORK/prod.c"
if ! $CC_PROBE $CSTD_PROBE -Wall -Wextra -I"$INC_DIR" -c "$WORK/prod.c" \
     -o "$WORK/prod.o" 2>"$WORK/prod.log"; then
	bad "production build of the fixture failed"
	sed 's/^/      /' "$WORK/prod.log"
elif nm "$WORK/prod.o" 2>/dev/null | grep -q 'goo_testcase_hit'; then
	bad "a production build references goo_testcase_hit; the marker must be inert"
else
	echo "    ok: the marker is inert in a production build"
fi

# --- Row 2: the argument stays type-checked in every build ----------------
# (void)sizeof(x) is what keeps this true. A marker whose expression rotted
# must not survive in the build where it expands to nothing.
sed 's/GOO_TESTCASE(n > 100)/GOO_TESTCASE(n > no_such_variable)/' "$WORK/prod.c" > "$WORK/rot.c"
if $CC_PROBE $CSTD_PROBE -Wall -Wextra -I"$INC_DIR" -c "$WORK/rot.c" \
   -o "$WORK/rot.o" 2>/dev/null; then
	bad "a marker naming an undefined variable compiled in a production build"
else
	echo "    ok: a marker whose expression rotted fails the production build"
fi

# --- Coverage helper -------------------------------------------------------
# Builds with --coverage -DGOO_COVERAGE, runs with the given argv, converts the
# counters with gcov, then asks the report. Echoes the report's stdout.
cov_run() {  # extra-argv ("" or "both")
	rm -rf "$WORK/cov"; mkdir -p "$WORK/cov"
	write_fixture "$WORK/cov/fixture.c"
	if ! ( cd "$WORK/cov" && $CC_PROBE $CSTD_PROBE -Wall -Wextra -I"$INC_DIR" \
	        -DGOO_COVERAGE --coverage fixture.c -o fixture ) 2>"$WORK/cov.log"; then
		echo "BUILD_FAILED"
		return
	fi
	( cd "$WORK/cov" && ./fixture $1 >/dev/null 2>&1 )
	# No gcov call here on purpose. The report runs gcov itself, with the same
	# --json-format invocation coverage_corpus.sh uses, so this probe exercises
	# the real reading path rather than a text format nothing else produces.
	# --cc is not decoration: gcov refuses a .gcno from a different major gcc,
	# and refuses it silently. CI runs `make CC=gcc-14` while plain gcov there
	# is gcov-13, which is how this probe failed on CI after passing twice
	# locally. The report derives the matching gcov from what it is told here.
	bash "$REPORT" --gcov-dir "$WORK/cov" --cc "$CC_PROBE" --source "$WORK/cov/fixture.c" 2>&1
}

# --- Row 3: driven ONE way -> the report names it -------------------------
out="$(cov_run "")"
if [ "$out" = "BUILD_FAILED" ]; then
	bad "the coverage build of the fixture failed"
	sed 's/^/      /' "$WORK/cov.log"
elif printf '%s' "$out" | grep -q "BROKEN"; then
	# NOT a marker defect. The report could not measure at all, and saying
	# "N problem(s) in GOO_TESTCASE" here is what sent the CI failure of
	# 2026-08-20 looking for a bug in the macro instead of at the toolchain.
	bad "could not measure: the report refused before judging any marker"
	printf '%s\n' "$out" | sed 's/^/      /'
elif ! printf '%s' "$out" | grep -q "1 not exercised both ways"; then
	bad "a marker driven only one way was not reported"
	printf '%s\n' "$out" | sed 's/^/      /'
elif ! printf '%s' "$out" | grep -q "fixture.c:$MARKER_LINE"; then
	bad "the report does not name the marker's file and line"
	printf '%s\n' "$out" | sed 's/^/      /'
else
	echo "    ok: a marker driven one way is reported, with its file and line"
fi

# --- Row 4: driven BOTH ways -> the report clears it ----------------------
out="$(cov_run both)"
if [ "$out" = "BUILD_FAILED" ]; then
	bad "the coverage build of the fixture failed"
elif printf '%s' "$out" | grep -q "BROKEN"; then
	bad "could not measure: the report refused before judging any marker"
	printf '%s\n' "$out" | sed 's/^/      /'
elif ! printf '%s' "$out" | grep -q "0 not exercised both ways"; then
	bad "a marker driven both ways is still reported as incomplete"
	printf '%s\n' "$out" | sed 's/^/      /'
else
	echo "    ok: a marker driven both ways clears"
fi

# --- Row 5: no data is not a clean sheet ----------------------------------
# The row that stops this whole mechanism from reporting success on a run that
# never happened -- the failure mode coverage_corpus.sh names four times.
rm -rf "$WORK/empty"; mkdir -p "$WORK/empty"
write_fixture "$WORK/empty/fixture.c"
bash "$REPORT" --gcov-dir "$WORK/empty" --source "$WORK/empty/fixture.c" > "$WORK/empty.out" 2>&1
rc=$?
# The status alone is NOT enough here, and the self-test proved it: with no
# gcov data the marker count is also zero, so the "no markers" guard fires and
# exits 2 as well. Deleting the no-data guard left the status unchanged and the
# row stayed green under the mutation written to catch it. Assert WHICH guard
# spoke, the way the goo_check probe learned to assert the verdict line rather
# than a substring of its own fixture.
if [ "$rc" -ne 2 ]; then
	bad "the report exited $rc with no gcov data present; expected 2 (could not measure)"
	sed 's/^/      /' "$WORK/empty.out"
elif ! grep -qF "gcov emitted no JSON for any object" "$WORK/empty.out"; then
	bad "the report exited 2 without naming missing gcov data as the cause"
	sed 's/^/      /' "$WORK/empty.out"
else
	echo "    ok: no gcov data is refused by name, not reported as zero incomplete"
fi

# --- Row 6: a gcov that emits nothing must read as a toolchain mismatch ----
# The CI failure this row exists for: gcov-13 silently refused gcc-14's .gcno,
# the report said "no JSON", and the probe reported it as a defect in the
# marker. A wrong tool must accuse the tool, by name and version.
rm -rf "$WORK/mm"; mkdir -p "$WORK/mm"
write_fixture "$WORK/mm/fixture.c"
( cd "$WORK/mm" && $CC_PROBE $CSTD_PROBE -I"$INC_DIR" -DGOO_COVERAGE --coverage \
    fixture.c -o fixture >/dev/null 2>&1 && ./fixture >/dev/null 2>&1 )
GCOV=/bin/true bash "$REPORT" --gcov-dir "$WORK/mm" --cc "$CC_PROBE" \
    --source "$WORK/mm/fixture.c" > "$WORK/mm.out" 2>&1
rc=$?
if [ "$rc" -ne 2 ]; then
	bad "a gcov that emits nothing exited $rc; expected 2 (could not measure)"
	sed 's/^/      /' "$WORK/mm.out"
elif ! grep -qF "VERSION MISMATCH" "$WORK/mm.out"; then
	bad "the refusal does not name a version mismatch as the usual cause"
	sed 's/^/      /' "$WORK/mm.out"
elif ! grep -qF "gcov:" "$WORK/mm.out"; then
	bad "the refusal does not name the gcov binary it used"
	sed 's/^/      /' "$WORK/mm.out"
else
	echo "    ok: a gcov that emits nothing accuses the toolchain, by name"
fi

if [ "$fails" -ne 0 ]; then
	echo "$PROBE: FAIL ($fails problem(s) in GOO_TESTCASE or $REPORT)"
	exit 1
fi
echo "$PROBE: PASS (6 contract rows over $INC_DIR/goo_assert.h)"
exit 0
