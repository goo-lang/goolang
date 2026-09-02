#!/usr/bin/env bash
# pthread-return-probe -- the MISRA Rule 17.7 pthread-return-value gate.
#
# docs/misra/README.md counts 1,516 kept Rule 17.7 violations (a function's
# return value used by nothing), and 840 of those are an unchecked
# pthread_* return -- the single largest slice of that backlog, and also
# Holzmann's "Power of Ten" rule 7 (check the return value of every
# function, or explicitly discard it with a cast). This probe is a
# SHRINK-ONLY ratchet on that slice: the count recorded per file in
# scripts/pthread-return-baseline.txt can only fall. Task 2 wires it into
# the Makefile; this probe does not run in verify-core yet.
#
# The counting engine is tools/pthread_return_count.py -- parenthesis
# matching across lines is awkward in awk/sed, so it lives in a language
# built for it. ITS DOCSTRING IS THE COUNTING RULE, in full, including the
# one deliberate widening beyond the literal "token before the identifier"
# test (preprocessor directive lines are blanked like comments, because this
# codebase gates almost every real pthread_* call behind
# "#ifdef GOO_PLATFORM_UNIX", which is unconditionally true on the Linux
# build this repo targets). Read that file before changing either one.
#
# WHY THIS COUNTER'S NUMBER DOES NOT MATCH CPPCHECK'S 840 (measured
# 2026-09-02, tools/pthread_return_count.py, this branch's src/ -- 68 files,
# 6 calling a pthread_* function -- git-identical to main):
#
#   1. SCOPE HAS SHRUNK SINCE THE 840 WAS MEASURED. docs/misra/README.md
#      dates that scan 2026-08-14. Commit ff37ea3 ("chore: quarantine the
#      subsystems that link into nothing"), dated 2026-08-17 -- three days
#      LATER -- moved 40 files (30,732 lines) that compiled into no binary
#      out of src/ into attic/src/, including entire pthread-heavy
#      subsystems: attic/src/runtime/actor_system.c (143 pthread_
#      mentions), attic/src/concurrency/shared_variables.c (126),
#      attic/src/async/async_resource.c (123), .../async_streams.c (119),
#      attic/src/concurrency/structured_concurrency.c (107), and 18 more
#      files, roughly 1,000 pthread_ mentions between them. The 840 almost
#      certainly counts a great many call sites that no longer live in
#      src/ at all. This counter, like the misra scan itself if re-run
#      today, only ever sees the CURRENT tree.
#   2. RULE ATTRIBUTION: cppcheck's Rule 17.7 check
#      (/usr/share/Cppcheck/addons/misra.py, misra_17_7) fires only when it
#      knows the call's return TYPE, which for a library function comes from
#      its --library=posix/gnu config (/usr/share/Cppcheck/cfg/posix.cfg).
#      That file declares pthread_mutex_lock/unlock/init/destroy/trylock,
#      pthread_create/join/detach and pthread_cond_signal/broadcast, but NOT
#      pthread_cond_wait, pthread_cond_init or pthread_cond_destroy.
#      VERIFIED directly (cppcheck 2.21.1, `cppcheck --dump` +
#      `python3 misra.py <dump>` against src/runtime/sync.c, no
#      --rule-texts needed for detection): the three pthread_mutex_*
#      discards there (lines 32, 42, 54) and one pthread_cond_broadcast
#      (line 113) are reported as misra-c2012-17.7; the three pthread_cond_
#      calls with no known signature (cond_init line 79, cond_destroy line
#      89, cond_wait line 131) are reported as misra-c2012-17.3 (implicit
#      declaration) instead -- a DIFFERENT rule, already documented as a
#      known checker limitation in docs/misra/README.md's "Known limits"
#      section. Those calls are real, discarded, unchecked pthread returns
#      by any reading, but the 840 (a Rule-17.7-only slice) does not include
#      them, while this counter -- one function family, no signature
#      lookup required -- does. src/runtime/channels.c is dominated by
#      exactly these three functions (its 17 counted calls are almost all
#      pthread_cond_wait), so this is not a marginal effect here.
#   Net effect: this counter and the 840 disagree in BOTH directions on
#   overlapping ground, on top of measuring different trees. Chasing parity
#   would mean recreating a stale scan; the goal here is a ratchet on
#   TODAY'S source, which is what this ties to.
#
# THE GATE: red when a file's count in the current tree is ABOVE its row in
# the baseline, or when a file with a count above zero has NO row in the
# baseline at all. Never red for a file that shrank or disappeared -- the
# baseline is tightened by --update-baseline, by a human, after a fix.
#
# Usage:
#   scripts/pthread_return_probe.sh                    # gate (exit 1 on red)
#   scripts/pthread_return_probe.sh --update-baseline   # accept current counts
#   scripts/pthread_return_probe.sh --self-test         # prove it can go red
set -u

PROBE="pthread-return-probe"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
COUNTER="$REPO_ROOT/tools/pthread_return_count.py"
# PTHREAD_RETURN_ROOT overrides which tree gets scanned (the self-test points
# it at a scratch copy). PTHREAD_RETURN_BASELINE overrides which baseline
# file it is compared against -- the self-test always uses a scratch
# baseline it writes itself, never the real one.
ROOT="${PTHREAD_RETURN_ROOT:-$REPO_ROOT}"
BASELINE="${PTHREAD_RETURN_BASELINE:-$REPO_ROOT/scripts/pthread-return-baseline.txt}"

# list_files: same file-scope rule as scripts/assertion_density_probe.sh --
# every *.c under src/, except *.tab.c and lex.yy.c. Used here only to print
# the file count in the verdict line; tools/pthread_return_count.py applies
# the identical rule for the actual counting.
list_files() { # $1 = a directory containing an src/ tree
	find "$1" -type f -name '*.c' ! -name '*.tab.c' ! -name 'lex.yy.c' -print0 2>/dev/null
}

# counter_rows / counter_total: split the Python counter's own output (one
# call, captured by the caller into $1) into the per-file rows (no TOTAL
# line) and the total. Callers that need both values run the counter once
# and pass its output to both functions, rather than running it twice.
counter_rows() {
	printf '%s\n' "$1" | grep -v '^TOTAL	'
}
counter_total() {
	printf '%s\n' "$1" | grep '^TOTAL	' | cut -f2
}

# baseline_total: sum of the count column across every real row of a
# baseline file (comments and blank lines skipped).
baseline_total() {
	grep -vE '^[[:space:]]*(#|$)' "$1" 2>/dev/null | awk -F'\t' '{s+=$2+0} END{print s+0}'
}

# ---------------------------------------------------------------------------
# --self-test: prove the gate can go both red and green. Uses the smallest
# src/*.c file that ALREADY calls a pthread_* function (today that is
# src/runtime/sync_shim.c, 121 lines) so the injected line lands inside real,
# representative code rather than a synthetic fixture.
#
# Four checks against a baseline captured from the UNMODIFIED copy:
#   1. inject one bare `pthread_mutex_lock(&m);`        -> must be RED
#   2. inject a CHECKED call instead (`if (... != 0)`)   -> must be GREEN
#   3. inject a `(void)`-cast call instead               -> must be GREEN
#   4. the unmodified copy again (the positive control)  -> must be GREEN
# Each injection's landing is verified with grep before its result is
# trusted -- a sed that silently matched nothing would otherwise read
# exactly like a passing probe.
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--self-test" ]; then
	SELF="$PROBE --self-test"
	W="$(mktemp -d)"
	trap 'rm -rf "$W"' EXIT
	bad_self=0

	pick_source_file() {
		local best="" best_n="" f n
		while IFS= read -r -d '' f; do
			grep -q 'pthread_' "$f" || continue
			n="$(wc -l <"$f")"
			if [ -z "$best_n" ] || [ "$n" -lt "$best_n" ]; then
				best="$f"
				best_n="$n"
			fi
		done < <(list_files "$REPO_ROOT/src")
		printf '%s' "$best"
	}

	SRC_FILE="$(pick_source_file)"
	if [ -z "$SRC_FILE" ]; then
		echo "$SELF: FAIL (no src/*.c file calls a pthread_ function -- nothing to self-test against)"
		exit 1
	fi
	BASENAME="$(basename "$SRC_FILE")"
	echo "    using $(realpath --relative-to="$REPO_ROOT" "$SRC_FILE" 2>/dev/null || echo "$SRC_FILE") as the self-test fixture"

	mk() { rm -rf "$W/t"; mkdir -p "$W/t/src"; cp "$SRC_FILE" "$W/t/src/$BASENAME"; }
	run() { PTHREAD_RETURN_ROOT="$W/t" PTHREAD_RETURN_BASELINE="$W/base.txt" "$0" >"$W/out.log" 2>&1; }

	# Baseline: the UNMODIFIED copy's own counts. Written directly (not via
	# --update-baseline) so this self-test never depends on that mode.
	mk
	counter_rows "$(python3 "$COUNTER" "$W/t")" >"$W/base.txt"
	base_count="$(grep -F "src/$BASENAME" "$W/base.txt" | cut -f2)"
	[ -n "$base_count" ] || base_count=0
	echo "    ok: baseline for src/$BASENAME captured at $base_count"

	inject_after_first_block_open() { # $1 = file, $2 = line to insert
		local n
		n="$(grep -nE '\)[[:space:]]*\{[[:space:]]*$' "$1" | head -n1 | cut -d: -f1)"
		[ -n "$n" ] || return 1
		sed -i "${n}a\\    ${2}" "$1"
	}

	# 1. One bare unchecked call -> RED.
	mk
	TARGET="$W/t/src/$BASENAME"
	if ! inject_after_first_block_open "$TARGET" 'pthread_mutex_lock(&m);'; then
		echo "$SELF: FAIL (no '...) {' line in $BASENAME to inject into)"
		exit 1
	fi
	if ! grep -qF 'pthread_mutex_lock(&m);' "$TARGET"; then
		echo "$SELF: FAIL (the unchecked-call injection did not land in $TARGET)"
		exit 1
	fi
	echo "    ok: the unchecked-call injection landed"
	if run; then
		echo "$SELF: FAIL (stayed GREEN after adding one unchecked pthread_mutex_lock)"
		sed 's/^/        /' "$W/out.log"
		bad_self=1
	else
		echo "    ok: one unchecked pthread_mutex_lock is RED"
	fi

	# 2. A CHECKED call instead -> GREEN.
	mk
	TARGET="$W/t/src/$BASENAME"
	inject_after_first_block_open "$TARGET" 'if (pthread_mutex_lock(&m) != 0) { }'
	if ! grep -qF 'if (pthread_mutex_lock(&m) != 0) { }' "$TARGET"; then
		echo "$SELF: FAIL (the checked-call injection did not land in $TARGET)"
		bad_self=1
	elif run; then
		echo "    ok: a checked pthread_mutex_lock (if (...) != 0) is GREEN"
	else
		echo "$SELF: FAIL (a checked pthread_mutex_lock went RED: it must not be counted)"
		sed 's/^/        /' "$W/out.log"
		bad_self=1
	fi

	# 3. A (void)-cast call instead -> GREEN (MISRA 17.7 treats this as
	#    compliant, and this rule agrees for the same reason).
	mk
	TARGET="$W/t/src/$BASENAME"
	inject_after_first_block_open "$TARGET" '(void)pthread_mutex_lock(&m);'
	if ! grep -qF '(void)pthread_mutex_lock(&m);' "$TARGET"; then
		echo "$SELF: FAIL (the (void)-cast injection did not land in $TARGET)"
		bad_self=1
	elif run; then
		echo "    ok: a (void)-cast pthread_mutex_lock is GREEN"
	else
		echo "$SELF: FAIL (a (void)-cast pthread_mutex_lock went RED: it must not be counted)"
		sed 's/^/        /' "$W/out.log"
		bad_self=1
	fi

	# 4. Unmodified copy again: the positive control -- without it, a
	#    harness that is red for every input would look identical to a
	#    working negative control.
	mk
	if run; then
		echo "    ok: an unmodified copy is GREEN against its own baseline"
	else
		echo "$SELF: FAIL (an unmodified copy is RED -- the harness is broken, not the source)"
		sed 's/^/        /' "$W/out.log"
		bad_self=1
	fi

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
# --update-baseline: accept the current per-file counts. Never invoked by
# the gate itself -- a human runs this, deliberately, after checking a
# pthread_* return or otherwise shrinking the count.
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--update-baseline" ]; then
	raw="$(python3 "$COUNTER" "$ROOT")"
	rows="$(counter_rows "$raw")"
	total="$(counter_total "$raw")"
	nfiles="$(list_files "$ROOT/src" | tr '\0' '\n' | grep -c .)"
	{
		echo "# pthread-return-probe baseline. SHRINK-ONLY."
		echo "#"
		echo "# One line per file with an unchecked pthread_* return: <path><TAB><count>."
		echo "# The counting rule is the docstring of tools/pthread_return_count.py,"
		echo "# applied via scripts/pthread_return_probe.sh. Regenerate with:"
		echo "#   scripts/pthread_return_probe.sh --update-baseline"
		echo "#"
		echo "# MEASURED $(date -u +%Y-%m-%d), tools/pthread_return_count.py (this file's"
		echo "# counting rule), git HEAD $(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown):"
		echo "# $total unchecked pthread_* returns across $nfiles src/*.c files, $(printf '%s\n' "$rows" | grep -c .) of them nonzero."
		echo "#"
		echo "# Next to cppcheck's 840 (docs/misra/README.md, measured 2026-08-14, the"
		echo "# pthread_* slice of MISRA Rule 17.7 kept after deviation D-02): the two"
		echo "# numbers are NOT expected to match, and the header of"
		echo "# scripts/pthread_return_probe.sh explains why in detail -- in short, the"
		echo "# 840 was measured on a src/ tree three days OLDER than the 2026-08-17"
		echo "# quarantine that moved roughly 1,000 further pthread_ mentions out of"
		echo "# src/ into attic/src/, and cppcheck's Rule 17.7 cannot see a"
		echo "# pthread_cond_wait/_init/_destroy call at all (it has no signature for"
		echo "# them and reports Rule 17.3 instead), while this counter treats the"
		echo "# whole pthread_* family as one function group."
		echo "#"
		echo "# The gate is red when a file's count here rises above its row, or a file"
		echo "# with a count above zero has no row at all. It never writes this file"
		echo "# itself; only --update-baseline does."
		printf '%s\n' "$rows"
	} >"$BASELINE"
	echo "$PROBE: baseline updated -- $total unchecked pthread_* returns across $nfiles files ($BASELINE)"
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

raw="$(python3 "$COUNTER" "$ROOT")"
cur_total="$(counter_total "$raw")"
[ -n "$cur_total" ] || cur_total=0
nfiles="$(list_files "$ROOT/src" | tr '\0' '\n' | grep -c .)"
base_total="$(baseline_total "$BASELINE")"

cur_rows_file="$(mktemp)"
trap 'rm -f "$cur_rows_file"' EXIT
counter_rows "$raw" >"$cur_rows_file"

fails="$(awk -F'\t' '
	FNR==NR {
		if ($0 ~ /^#/ || $0 ~ /^[[:space:]]*$/) { next }
		base[$1] = $2
		next
	}
	{
		path = $1
		cur = $2 + 0
		if (path in base) {
			b = base[path] + 0
			if (cur > b) { print path "\t" cur "\t" b }
		} else {
			print path "\t" cur "\t-"
		}
	}
' "$BASELINE" "$cur_rows_file")"

verdict="($cur_total unchecked pthread returns in src/, $nfiles files, baseline $base_total)"

if [ -n "$fails" ]; then
	echo "$PROBE: FAIL $verdict"
	printf '%s\n' "$fails" | while IFS="$(printf '\t')" read -r path cur base; do
		echo "  FAIL: $path has $cur (baseline $base)"
	done
	rm -f "$cur_rows_file"
	trap - EXIT
	exit 1
fi

rm -f "$cur_rows_file"
trap - EXIT
echo "$PROBE: PASS $verdict"
exit 0
