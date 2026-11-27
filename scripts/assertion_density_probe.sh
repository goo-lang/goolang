#!/usr/bin/env bash
# assertion-density-probe -- the Power of Ten assertion-density gate.
#
# Gerard J. Holzmann, "The Power of Ten -- Rules for Developing Safety-Critical
# Code" (talk and IEEE Computer, June 2006), rule 5: assertions should check
# for anomalous conditions, and a healthy body of code checks itself at a rate
# of roughly TWO PERCENT of its executable lines. That 2% is the TARGET this
# gate is measured against. It is not the pass bar today -- the pass bar is
# "never go backward" (below) -- but every number this probe prints is
# comparable to it.
#
# COUNTING RULE (read this before changing the numbers below):
#   FILES:       every *.c under src/, except *.tab.c (bison output) and
#                lex.yy.c (flex output). Files under include/ are not counted.
#   ASSERTION LINE: a line containing the whole word "assert", "static_assert",
#                "GOO_ASSERT" or "goo_assert" (word-bounded, so "GOO_ASSERT"
#                does not also satisfy the lowercase "assert" pattern), whose
#                first non-blank characters are not "//" and not "*".
#   EXECUTABLE LINE: a line that is not blank, and whose first non-blank
#                characters are not "//", "/*", "*" or "#".
#   Both rules look only at the line's own leading characters -- neither one
#   tracks whether a line sits inside a multi-line /* */ comment. That is a
#   known, deliberate approximation: it is what "first non-blank characters"
#   means, verbatim, and it is cheap and exact enough that the same script run
#   twice on the same tree gives the same two numbers.
#
# MEASURED 2026-09-02 on this branch (69 *.c files under src/, minus
# parser.tab.c = 68 counted files): 23 assertion lines, 40,376 executable
# lines -- 0.057% density. The verification-tooling-next-steps plan (task 23)
# quotes 23 of 40,417 for the same 0.057%. The assertion count matches
# exactly; the executable-line count is 41 lines (0.1%) lower here. This
# branch's src/ is git-identical to main (`git diff main...HEAD -- src/`
# is empty), so the gap is not something this branch changed -- most likely
# the plan's number was taken from a slightly different commit of src/ than
# this branch's HEAD. Per the task brief, the MEASURED count is the baseline
# on this branch: see scripts/assertion-density-baseline.txt.
#
# THE GATE: the baseline can only increase. Red when the CURRENT assertion
# count is below the baseline's, or when the current density (assertions /
# executable lines) is below the baseline's density. It never writes the
# baseline itself -- see --update-baseline.
#
# Usage:
#   scripts/assertion_density_probe.sh                    # gate (exit 1 on red)
#   scripts/assertion_density_probe.sh --update-baseline   # accept current counts
#   scripts/assertion_density_probe.sh --self-test         # prove it can go red
set -u

PROBE="assertion-density-probe"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# ASSERTION_DENSITY_ROOT overrides which tree's src/ gets scanned (the
# self-test points it at a scratch copy). ASSERTION_DENSITY_BASELINE defaults
# to the committed file. The self-test overrides it with a temporary file that
# it builds from its own measurement.
ROOT="${ASSERTION_DENSITY_ROOT:-$REPO_ROOT}"
BASELINE="${ASSERTION_DENSITY_BASELINE:-$REPO_ROOT/scripts/assertion-density-baseline.txt}"

# ---------------------------------------------------------------------------
# The counting rule, factored so the gate, --update-baseline and --self-test
# all measure the exact same thing.
# ---------------------------------------------------------------------------

is_positive_int() { # $1 = candidate value; a whole number above zero
	case "$1" in
		''|*[!0-9]*) return 1 ;;
	esac
	[ "$1" -gt 0 ]
}

list_files() { # $1 = a directory containing an src/ tree (this fn is given the src/ dir itself)
	find "$1" -type f -name '*.c' ! -name '*.tab.c' ! -name 'lex.yy.c' -print0 2>/dev/null | LC_ALL=C sort -z
}

# Prints "<assertions> <executable-lines> <files>" for the src/ dir given.
count_dir() {
	local dir="$1" nfiles counts
	nfiles="$(list_files "$dir" | tr '\0' '\n' | grep -c .)"
	counts="$(list_files "$dir" | xargs -0 awk '
		{
			line = $0
			trimmed = line
			sub(/^[ \t]+/, "", trimmed)
			is_blank      = (trimmed == "")
			is_slashslash = (trimmed ~ /^\/\//)
			is_slashstar  = (trimmed ~ /^\/\*/)
			is_star       = (trimmed ~ /^\*/)
			is_hash       = (trimmed ~ /^#/)
			if (!is_blank && !is_slashslash && !is_slashstar && !is_star && !is_hash) {
				execn++
			}
			if (!is_slashslash && !is_star) {
				if (trimmed ~ /(^|[^A-Za-z0-9_])(assert|static_assert|GOO_ASSERT|goo_assert)([^A-Za-z0-9_]|$)/) {
					assertn++
				}
			}
		}
		END { printf "%d %d\n", assertn + 0, execn + 0 }
	')"
	[ -n "$counts" ] || counts="0 0"
	printf '%s %s\n' "$counts" "$nfiles"
}

# Finds the first assertion line in the given src/ dir. Sets FOUND_FILE and
# FOUND_LINE; returns 1 if the tree has none (only possible on a broken copy).
find_assertion_line() {
	FOUND_FILE=""
	FOUND_LINE=""
	local f ln
	while IFS= read -r -d '' f; do
		ln="$(awk '
			{
				line = $0
				trimmed = line
				sub(/^[ \t]+/, "", trimmed)
				is_slashslash = (trimmed ~ /^\/\//)
				is_star       = (trimmed ~ /^\*/)
				if (!is_slashslash && !is_star) {
					if (trimmed ~ /(^|[^A-Za-z0-9_])(assert|static_assert|GOO_ASSERT|goo_assert)([^A-Za-z0-9_]|$)/) {
						print NR
						exit
					}
				}
			}
		' "$f")"
		if [ -n "$ln" ]; then
			FOUND_FILE="$f"
			FOUND_LINE="$ln"
			return 0
		fi
	done < <(list_files "$1")
	return 1
}

# ---------------------------------------------------------------------------
# --self-test: the probe that refuses a density decrease must show it can
# detect one. The self-test measures the CURRENT tree first and builds every
# baseline it uses from that measurement, in a temporary file. It never reads
# scripts/assertion-density-baseline.txt, so a PR that adds code (and so moves
# the measurement) needs no baseline edit for the self-test to pass. The
# committed file is only the floor that the gate itself checks, and a PR
# edits it only to raise that floor.
#
# Let A and E be the measured assertion and executable-line counts.
#   control  baseline "A E" on the unmodified tree              -> GREEN
#   control  baseline "A-1 E" (committed floor below measure)    -> GREEN
#   fault    one assertion line removed from a copy, "A E"       -> RED
#   fault    baseline "A+1 E" (measured count one below floor)   -> RED
#   fault    baseline "A E-1" (count equal, density below floor) -> RED
#   fault    baseline "0 0" (a zero baseline verifies nothing)   -> RED
# The controls come first: without them, a harness that is red for every
# input would look identical to a working fault case.
#
# The mutation is verified to have LANDED (count dropped by exactly one)
# before its red result is trusted: a sed that silently matched nothing would
# otherwise read exactly like a passing probe.
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--self-test" ]; then
	SELF="$PROBE --self-test"
	W="$(mktemp -d)"
	trap 'rm -rf "$W"' EXIT
	bad_self=0

	meas="$(count_dir "$REPO_ROOT/src")"
	A="${meas%% *}"
	rest="${meas#* }"
	E="${rest%% *}"
	if ! is_positive_int "$A" || ! is_positive_int "$E" || [ "$E" -lt 2 ]; then
		echo "$SELF: FAIL (measured '$A' assertions / '$E' executable lines -- the tree is empty or the counter is broken)"
		exit 1
	fi
	echo "    measured: $A assertions / $E executable lines"

	# run_with <root> <baseline "a e"> : run the gate on <root> against a
	# temporary baseline file written from the given pair.
	run_with() {
		printf '%s\n' "$2" >"$W/baseline.txt"
		ASSERTION_DENSITY_ROOT="$1" ASSERTION_DENSITY_BASELINE="$W/baseline.txt" "$0" >"$W/out.log" 2>&1
	}
	expect_green() { # $1 = label, $2 = root, $3 = baseline pair
		if run_with "$2" "$3"; then
			echo "    ok: $1 is GREEN"
		else
			echo "$SELF: FAIL ($1 is RED -- the harness is broken, not the tree)"
			sed 's/^/        /' "$W/out.log"
			bad_self=1
		fi
	}
	expect_red() { # $1 = label, $2 = root, $3 = baseline pair
		if run_with "$2" "$3"; then
			echo "$SELF: FAIL ($1 stayed GREEN)"
			sed 's/^/        /' "$W/out.log"
			bad_self=1
		else
			echo "    ok: $1 is RED"
		fi
	}

	# Controls.
	expect_green "the unchanged tree against a baseline equal to the measurement ($A $E)" "$REPO_ROOT" "$A $E"
	expect_green "the unchanged tree against a baseline below the measurement ($((A - 1)) $E)" "$REPO_ROOT" "$((A - 1)) $E"

	# Fault: remove one assertion line from a copy.
	mkdir -p "$W/t"
	cp -r "$REPO_ROOT/src" "$W/t/src"
	if ! find_assertion_line "$W/t/src"; then
		echo "$SELF: FAIL (no assertion line found in the copy -- cannot inject)"
		exit 1
	fi
	sed -i "${FOUND_LINE}d" "$FOUND_FILE"
	after_line="$(count_dir "$W/t/src")"
	after_assert="${after_line%% *}"
	if [ "$after_assert" -ne "$((A - 1))" ]; then
		echo "$SELF: FAIL (deleting one assertion line moved the count $A -> $after_assert, wanted $((A - 1)))"
		bad_self=1
	else
		echo "    ok: the mutated copy has exactly one fewer assertion line ($A -> $after_assert)"
		expect_red "a copy with one assertion line removed against the measurement ($A $E)" "$W/t" "$A $E"
	fi

	# Faults on the baseline side, against the unchanged tree.
	expect_red "a baseline one assertion above the measurement ($((A + 1)) $E)" "$REPO_ROOT" "$((A + 1)) $E"
	expect_red "a baseline with the same count and a higher density ($A $((E - 1)))" "$REPO_ROOT" "$A $((E - 1))"
	# A baseline of 0 0 is not a real check: base_assert=0 can never read as
	# "below baseline", and the density compare is skipped when base_exec is 0.
	expect_red "a baseline of 0 0" "$REPO_ROOT" "0 0"

	rm -rf "$W"
	trap - EXIT
	if [ "$bad_self" -ne 0 ]; then
		echo "$SELF: FAIL"
		exit 1
	fi
	echo "$SELF: PASS"
	exit 0
fi

# ---------------------------------------------------------------------------
# --update-baseline: accept the current counts. Never invoked by the gate
# itself -- a human or a deliberate commit runs this.
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--update-baseline" ]; then
	line="$(count_dir "$ROOT/src")"
	a="${line%% *}"
	rest="${line#* }"
	e="${rest%% *}"
	{
		echo "# assertion-density gate baseline."
		echo "#"
		echo "# Format: two integers on one line -- <assertion lines> <executable lines>."
		echo "# Counting rule is in the header of scripts/assertion_density_probe.sh."
		echo "# Regenerate with:"
		echo "#   scripts/assertion_density_probe.sh --update-baseline"
		echo "#"
		echo "# The gate requires BOTH to hold or improve: the assertion count may not"
		echo "# fall, and the density (assertions / executable lines) may not fall. The"
		echo "# gate never writes this file itself."
		echo "$a $e"
	} >"$BASELINE"
	echo "$PROBE: baseline updated to $a assertions / $e executable lines ($BASELINE)"
	exit 0
fi

if [ -n "${1:-}" ]; then
	echo "usage: $0 [--update-baseline|--self-test]" >&2
	exit 2
fi

# ---------------------------------------------------------------------------
# The gate.
# ---------------------------------------------------------------------------
if [ ! -f "$BASELINE" ]; then
	echo "$PROBE: FAIL (no baseline at $BASELINE -- run --update-baseline once)"
	exit 1
fi

cur_line="$(count_dir "$ROOT/src")"
cur_assert="${cur_line%% *}"
cur_rest="${cur_line#* }"
cur_exec="${cur_rest%% *}"
cur_files="${cur_rest#* }"

base_line="$(grep -vE '^[[:space:]]*(#|$)' "$BASELINE" | head -n1)"
base_assert="$(printf '%s\n' "$base_line" | awk '{print $1}')"
base_exec="$(printf '%s\n' "$base_line" | awk '{print $2}')"
# A baseline of 0 0 (or anything not a whole number above zero) is not a real
# check: base_assert=0 can never be "below baseline", and the density compare
# below is skipped entirely when base_exec is 0. Reject both before the
# compare step runs, in the same shape as the parse failure above.
if [ -z "${base_assert:-}" ] || [ -z "${base_exec:-}" ] \
	|| ! is_positive_int "$base_assert" || ! is_positive_int "$base_exec"; then
	echo "$PROBE: FAIL (could not read two positive counts from $BASELINE: got '${base_assert:-}' '${base_exec:-}')"
	exit 1
fi

cur_density="$(awk -v a="$cur_assert" -v e="$cur_exec" 'BEGIN { if (e > 0) { printf "%.3f", (a / e) * 100 } else { printf "0.000" } }')"

verdict="($cur_assert assertions / $cur_exec executable lines = ${cur_density}%, src/ $cur_files files, baseline $base_assert / $base_exec)"

red=0
reason=""
if [ "$cur_assert" -lt "$base_assert" ]; then
	red=1
	reason="assertion count fell below baseline"
fi
# Density comparison by cross-multiplication, exact and immune to rounding:
#   cur_assert/cur_exec < base_assert/base_exec  <=>  cur_assert*base_exec < base_assert*cur_exec
if [ "$cur_exec" -gt 0 ] && [ "$base_exec" -gt 0 ]; then
	if [ $((cur_assert * base_exec)) -lt $((base_assert * cur_exec)) ]; then
		red=1
		[ -n "$reason" ] && reason="$reason; "
		reason="${reason}density fell below baseline"
	fi
fi

if [ "$red" -ne 0 ]; then
	echo "$PROBE: FAIL $verdict -- $reason"
	exit 1
fi
echo "$PROBE: PASS $verdict"
exit 0
