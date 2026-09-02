#!/usr/bin/env bash
# spin-deadlock-model-probe -- task 7 of the 2026-09-02 verification tooling
# plan. Connects tests/models/deadlock_detector.pml (tasks 5-6) as a
# verify-core gate over tests/models/run_deadlock_model.sh.
#
# THE CLAIM: the corrected protocol (FIX=1) finds 0 errors in a FULL,
# untruncated state-space search at K=1 -- the hardest threshold, task 6's
# README says a larger K can only remove aborts, so K=1 covers every K the C
# code could ship. N=2, N=3 and N=4 are all three RUN, because N=3 is the one
# unbalanced program (two senders, one receiver) where a genuine deadlock is
# reachable and the abort must fire correctly, while N=2 and N=4 must never
# abort at all.
#
# THE NEGATIVE CONTROL: FIX=0, the protocol as it ships today, must FAIL --
# and not merely exit non-zero. The runner's own pan output must name
# "assertion violated rd", so a row that fails for an unrelated reason (a
# build error, a different assertion) can never read as this control having
# fired. This is exactly the false-abort counterexample task 5 found: 519
# states, depth 331.
#
# A RUNNER EXIT OF 2 IS NEVER A PASS, in any row, including the negative
# control. run_deadlock_model.sh exits 2 for a truncated search or any other
# reason the run proves nothing (see its own header). This probe treats that
# exit identically everywhere: red, INCONCLUSIVE, with the runner's own
# reason quoted -- never silently retried, never counted as the row having
# "done its job by returning non-zero".
#
# WALL TIME, measured 2026-09-02 on this machine (32-core
# 7.1.12-200.fc44.x86_64, spin already built by a prior spin-bootstrap-probe
# run, so the figures below exclude that one-time build): the four gate rows
# together take about 4.6s (N=4 K=1 alone is the slowest at ~1.3s, of which
# only 0.32s is pan actually searching -- the rest is spin -a and the cc
# build of pan.c, paid on every row). The full `make spin-deadlock-model-probe`
# target, gate plus --self-test, takes about 18s. Both are comfortably inside
# the plan's 30s budget (bootstrap excluded), so N=4 stays an unconditional
# gate row -- SPIN_MODEL_FULL does not exist, because nothing needed moving
# behind it.
#
# Usage:
#   scripts/spin_deadlock_model_probe.sh              # the gate
#   scripts/spin_deadlock_model_probe.sh --self-test   # prove it can go red
set -u

PROBE="spin-deadlock-model-probe"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUNNER="$REPO_ROOT/tests/models/run_deadlock_model.sh"
MODEL_REL="tests/models/deadlock_detector.pml"

if [ ! -x "$RUNNER" ] && [ ! -f "$RUNNER" ]; then
	echo "$PROBE: FAIL (runner not found: $RUNNER)"
	exit 1
fi

# Bootstrap SPIN once, up front, outside the timed gate -- a from-scratch
# build (the first run on a machine) belongs to spin-bootstrap-probe's own
# budget, not this one. SPIN_BUILD_DIR is exported so every later call the
# runner itself makes to scripts/spin_bootstrap.sh (it makes one per row --
# see run_deadlock_model.sh) resolves to the SAME build directory and takes
# the fast "previously built, skip the build" path instead of re-verifying
# a tarball on every row.
export SPIN_BUILD_DIR="${SPIN_BUILD_DIR:-$REPO_ROOT/build/tools}"
SPIN_BIN="$(bash "$REPO_ROOT/scripts/spin_bootstrap.sh" 2>/dev/null | tail -1)"
if [ -z "${SPIN_BIN:-}" ] || [ ! -x "$SPIN_BIN" ]; then
	echo "$PROBE: FAIL (scripts/spin_bootstrap.sh gave no usable spin binary)"
	exit 1
fi
SPIN_RAW_VERSION="$("$SPIN_BIN" -V 2>&1 | head -1)"
SPIN_VERSION="$(printf '%s\n' "$SPIN_RAW_VERSION" | grep -oE 'Spin Version [0-9]+\.[0-9]+\.[0-9]+' | head -1)"
[ -n "$SPIN_VERSION" ] || SPIN_VERSION="$SPIN_RAW_VERSION"

# ---------------------------------------------------------------------------
# run_row: invoke the runner with the given positional arguments, capturing
# its combined stdout+stderr and exit status into ROW_OUT / ROW_RC. CC and
# MODEL_DEFS, if the caller sets them as a prefix assignment on the call
# (for example "MODEL_DEFS='-DDOUBLE_DEC=1' run_row 2 1 1 1"), reach the
# runner as environment variables, exactly as tests/models/README.md
# documents.
# ---------------------------------------------------------------------------
run_row() { # $1=N $2=K $3=DETECTOR $4=FIX
	ROW_OUT="$(bash "$RUNNER" "$1" "$2" "$3" "$4" 2>&1)"
	ROW_RC=$?
	ROW_VERDICT_LINE="$(printf '%s\n' "$ROW_OUT" | grep -m1 '^run_deadlock_model: ')"
}

report_inconclusive() { # $1=label
	echo "$PROBE: FAIL ($1 is INCONCLUSIVE)"
	echo "    $ROW_VERDICT_LINE"
	printf '%s\n' "$ROW_OUT" | grep -m1 'pan said:' | sed 's/^/    /'
}

# note_bad: remember the FIRST row that went wrong, so the closing verdict
# line can name it. Later failures still print in full above; only the
# first one drives the closing summary, the way a test suite names its
# first failure.
FIRST_BAD_LABEL=""
FIRST_BAD_KIND=""
FIRST_BAD_DETAIL=""
note_bad() { # $1=label $2=kind(FAIL|INCONCLUSIVE) $3=detail
	if [ -z "$FIRST_BAD_LABEL" ]; then
		FIRST_BAD_LABEL="$1"
		FIRST_BAD_KIND="$2"
		FIRST_BAD_DETAIL="$3"
	fi
}

# check_pass: the row must exit 0 -- a full search, 0 errors. Returns 0, 1
# (FAIL) or 2 (INCONCLUSIVE); ROW_OUT / ROW_RC are left set for a caller
# that wants to inspect them further (the self-test's mutant check does).
check_pass() {
	local label="$1"
	shift
	run_row "$@"
	case "$ROW_RC" in
	0)
		echo "    ok: $label -- 0 errors, full search"
		return 0
		;;
	2)
		report_inconclusive "$label"
		note_bad "$label" "INCONCLUSIVE" "$ROW_VERDICT_LINE"
		return 2
		;;
	*)
		echo "$PROBE: FAIL ($label exited $ROW_RC, want 0)"
		echo "    $ROW_VERDICT_LINE"
		note_bad "$label" "FAIL" "$ROW_VERDICT_LINE"
		return 1
		;;
	esac
}

# check_fail_named: the row must exit 1 AND its output must name the given
# substring -- not merely exit non-zero. This is what stops an unrelated
# build failure or a different assertion from reading as the negative
# control having fired.
check_fail_named() {
	local label="$1" substr="$2"
	shift 2
	run_row "$@"
	case "$ROW_RC" in
	1)
		if printf '%s\n' "$ROW_OUT" | grep -qF -- "$substr"; then
			echo "    ok: $label -- fails on '$substr', as required"
			return 0
		fi
		echo "$PROBE: FAIL ($label exited 1 but never printed '$substr')"
		echo "    $ROW_VERDICT_LINE"
		note_bad "$label" "FAIL" "$ROW_VERDICT_LINE (never printed '$substr')"
		return 1
		;;
	2)
		report_inconclusive "$label"
		note_bad "$label" "INCONCLUSIVE" "$ROW_VERDICT_LINE"
		return 2
		;;
	0)
		echo "$PROBE: FAIL ($label exited 0 -- the negative control must fail)"
		echo "    $ROW_VERDICT_LINE"
		note_bad "$label" "FAIL" "$ROW_VERDICT_LINE (exited 0)"
		return 1
		;;
	*)
		echo "$PROBE: FAIL ($label exited $ROW_RC, want 1)"
		echo "    $ROW_VERDICT_LINE"
		note_bad "$label" "FAIL" "$ROW_VERDICT_LINE"
		return 1
		;;
	esac
}

# run_gate: the four rows the plan's task 7 asks for, in order. Returns 0
# only when every row passed its own check.
run_gate() {
	FIRST_BAD_LABEL=""
	FIRST_BAD_KIND=""
	FIRST_BAD_DETAIL=""
	local overall=0 r

	check_pass "FIX=1 K=1 N=2" 2 1 1 1
	r=$?
	[ "$r" -ne 0 ] && overall=$r

	check_pass "FIX=1 K=1 N=3" 3 1 1 1
	r=$?
	[ "$r" -ne 0 ] && overall=$r

	check_pass "FIX=1 K=1 N=4" 4 1 1 1
	r=$?
	[ "$r" -ne 0 ] && overall=$r

	check_fail_named "FIX=0 N=2 K=1 (negative control)" "assertion violated rd" 2 1 1 0
	r=$?
	[ "$r" -ne 0 ] && overall=$r

	if [ "$overall" -eq 0 ]; then
		echo "$PROBE: PASS ($MODEL_REL, FIX=1 K=1 N=2,3,4 0 errors full search; FIX=0 N=2 K=1 fails rd; $SPIN_VERSION)"
		return 0
	fi

	if [ "$FIRST_BAD_KIND" = "INCONCLUSIVE" ]; then
		echo "$PROBE: FAIL ($MODEL_REL, $FIRST_BAD_LABEL is INCONCLUSIVE -- $FIRST_BAD_DETAIL)"
	else
		echo "$PROBE: FAIL ($MODEL_REL, $FIRST_BAD_LABEL -- $FIRST_BAD_DETAIL)"
	fi
	return 1
}

# ---------------------------------------------------------------------------
# --self-test: three checks, none of which trust the row logic above until
# it has shown it can go red as well as green.
#
#   (a) a mutant: MODEL_DEFS='-DDOUBLE_DEC=1' at N=2 K=1 through check_pass,
#       the SAME function the gate uses for that row. The mutant reinstates
#       the wake-time decrement beside the broadcaster's decrement, so the
#       count goes negative and `assert(nblocked > 0)` fires (task 6's
#       README: FAIL, 139 states). Verified FIRST that the define actually
#       reached the runner -- via the runner's own defs= field, which
#       check_pass leaves behind in ROW_OUT -- because a probe that went red
#       for the wrong reason (a stray CC problem, say) would look identical
#       to this check passing.
#   (b) an inconclusive run: CC='cc -DMEMLIM=1' for one row, through the
#       same check_pass. task 6's README shows this exits 2 with
#       "pan: reached -DMEMLIM bound" and 0 states explored -- never a pass.
#   (c) the positive control: run_gate itself, unmodified, must be green.
# ---------------------------------------------------------------------------
self_test() {
	local SELF="$PROBE --self-test"
	local bad_self=0

	echo "  -- (a) mutant MODEL_DEFS=-DDOUBLE_DEC=1 at N=2 K=1, through check_pass"
	MODEL_DEFS='-DDOUBLE_DEC=1' check_pass "FIX=1 K=1 N=2 (mutant DOUBLE_DEC=1)" 2 1 1 1
	local mutant_rc=$? mutant_out="$ROW_OUT"
	if ! printf '%s\n' "$mutant_out" | grep -q -- '-DDOUBLE_DEC=1'; then
		echo "$SELF: FAIL (the runner's defs= field never shows -DDOUBLE_DEC=1 -- the mutation did not reach it)"
		printf '%s\n' "$mutant_out" | grep -m1 "defs=" | sed 's/^/        /'
		bad_self=1
	elif [ "$mutant_rc" -eq 0 ]; then
		echo "$SELF: FAIL (MODEL_DEFS=-DDOUBLE_DEC=1 stayed GREEN at N=2 K=1 FIX=1)"
		bad_self=1
	else
		echo "    ok: the DOUBLE_DEC=1 define reached the runner (defs= shows it), and the row went red"
	fi

	echo "  -- (b) inconclusive control CC='cc -DMEMLIM=1' at N=2 K=1, through check_pass"
	CC='cc -DMEMLIM=1' check_pass "FIX=1 K=1 N=2 (CC=cc -DMEMLIM=1)" 2 1 1 1
	local inconclusive_rc=$?
	if [ "$inconclusive_rc" -ne 2 ]; then
		echo "$SELF: FAIL (CC='cc -DMEMLIM=1' returned $inconclusive_rc from check_pass, want 2 INCONCLUSIVE)"
		bad_self=1
	else
		echo "    ok: CC='cc -DMEMLIM=1' reads as INCONCLUSIVE, never a pass"
	fi

	echo "  -- (c) positive control: the four normal rows, unmodified"
	if run_gate; then
		echo "    ok: the unmodified rows are GREEN"
	else
		echo "$SELF: FAIL (the unmodified rows are RED -- the harness is broken, not the model)"
		bad_self=1
	fi

	if [ "$bad_self" -ne 0 ]; then
		echo "$SELF: FAIL"
		return 1
	fi
	echo "$SELF: PASS"
	return 0
}

if [ "${1:-}" = "--self-test" ]; then
	self_test
	exit $?
fi

if [ -n "${1:-}" ]; then
	echo "usage: $0 [--self-test]" >&2
	exit 2
fi

run_gate
exit $?
