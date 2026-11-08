#!/usr/bin/env bash
# warning-count-probe -- the Power of Ten warning-count gate.
#
# Gerard J. Holzmann, "The Power of Ten -- Rules for Developing Safety-Critical
# Code" (talk and IEEE Computer, June 2006): compile with all warnings on, and
# stop the build on one warning. Today the build is warning-heavy, so this
# gate does not yet stop on ONE warning -- it stops on an INCREASE. A later
# task (22) removes the warnings and turns this into a real -Werror gate.
#
# WHAT THIS DOES: rebuilds bin/goo and libgoo_runtime.a from clean, in a
# sandboxed build directory so the compiler the OTHER probes use is never
# touched, and counts the compiler warnings the rebuild produced. This
# probe writes only under build/warning-count, the temporary directory of its self-test,
# and one small lock file at build/.warning-count-probe.lock -- nowhere
# else in the tree, not even transiently.
#
# COUNTING RULE (read this before changing the numbers below):
#   A counted line contains the literal text "warning:". Two families of
#   line are excluded from the C count:
#     - BISON lines: tagged "[-Wother]" or "[-Wconflicts-sr]". These come
#       from a SEPARATE, dedicated bison run (see BISON COUNT below), are
#       counted and printed as their OWN number, but the gate does not
#       compare them to a baseline -- scripts/grammar-tripwire.sh already
#       owns the shift/reduce and useless-symbol conflict count, with its
#       own EXPECTED_SR/EXPECTED_RR tripwire. Counting the same number twice,
#       gated in two places with two different rules, would be worse than
#       counting it once. This filter also protects the C count on the rare
#       run where parser.y changed and the real bison rule fires as part of
#       an ordinary (non-forced) build -- see NO -B below.
#     - BUILD-TOOL lines such as "gmake[1]: warning: ... jobserver mode.":
#       GNU Make talking about its own job scheduling, not the compiler
#       talking about the source. Matched by "line starts with
#       (g)?make[N]: warning:" -- a real gcc or bison warning line always
#       starts with a source path instead.
#   Everything else that says "warning:" is a C warning, INCLUDING lines
#   with no "[-Wxxx]" tag at all (gcc does not attach a flag name to some
#   warnings it cannot be told to suppress individually, e.g. macro
#   redefinition -- "_XOPEN_SOURCE redefined" is the majority of those here).
#
# NO -B: an earlier version of this script rebuilt with `-B`
# (--always-make), which forces EVERY prerequisite of `lexer` to remake,
# including two prerequisites the Makefile does not scope to
# BUILDDIR/BINDIR/LIBDIR at all: the vendored NNG library
# (build/nng/lib/libnng.a, rebuilt from a `rm -rf` + cmake recipe) and the
# bison-generated src/parser/parser.tab.c/.h (written straight into the
# source tree). A task review (2026-09-02) found this UNSAFE: those two
# shared paths are also read by far-transport-test, far-transport-asan and
# the real lib/libgoo_runtime.a, all of which run inside the same `make -j
# verify-core`, so a concurrent run of this probe could delete libnng.a
# out from under another probe's link step. `-B` forced that delete-and-
# rebuild on EVERY run, whether or not the shared archive needed it.
#
# The fix: remove the sandbox directory before each run, then build with a
# PLAIN `make` (no -B). An empty sandbox makes every object under
# BUILDDIR/BINDIR/LIBDIR rebuild regardless, which is all this gate needs
# for a real "from clean" C-warning count. The vendored NNG library sits
# OUTSIDE the sandbox and is untouched by the sandbox wipe, so Make's
# ordinary dependency check reuses it as-is whenever it is already
# up to date relative to its own prerequisites (the tarball and the
# Makefile) -- exactly the shared-archive reuse a concurrent-safe probe
# needs, with no NNG-specific variable override required. The same
# ordinary dependency check also means this probe no longer touches
# src/parser/parser.tab.c/.h at all in the common case (bison's rule only
# fires when parser.y itself changed, which is true of any build, forced
# or not, and not a side effect of this probe).
#
# BISON COUNT: because a plain build usually will NOT re-run bison (the
# generated files are normally already up to date), this probe cannot read
# the bison warning count off the main build log. Instead it runs bison a
# SECOND time, on its own, with the same flags the Makefile's parser.y rule
# uses, but with its output pointed at a path inside the sandbox
# (build/warning-count/bison-check/), so nothing under src/ is written.
# This run exists purely to print a number; the gate never compares it to
# a baseline. The probe hashes the real parser.tab.c/.h before and after
# this run and FAILS loudly if either ever changes -- this must never
# happen, since the sandbox path is hardcoded, but the proof costs nothing
# and would catch a future bison invocation that regresses this.
#
# SHARED-BINARY PROOF: bin/goo and lib/libgoo_runtime.a are the two
# outputs every OTHER probe reads. This probe hashes both with sha256sum
# before and after its own build and FAILS if either ever changes --
# sha256, not a modification-time compare, because a rebuild fast enough to
# land inside one filesystem-mtime tick (1 second on most filesystems)
# could otherwise go unnoticed.
#
# CCACHE: kept, deliberately. `CC = $(CCACHE) gcc` already shadows plain
# `gcc` through ccache on a machine where ccache is installed (its bin
# directory is typically ahead of /usr/bin on PATH), and ccache REPLAYS the
# original compiler's diagnostics on a cache hit -- the warning count is the
# same with a warm cache as with a cold one. Measured 2026-09-02, after the
# NO -B fix: `make warning-count-probe` (both the gate and its --self-test)
# took 1.154 seconds with a warm cache, and 1.263 seconds under
# `CCACHE_DISABLE=1` (a genuine cold compile of every file, proved by 418%
# CPU against 127% on the warm run) -- both counted 75 C and 8 bison
# warnings. Both times sit well inside the 3-minute budget
# parity_test.sh and this gate's own --self-test both exercise.
#
# BASELINE IS PER COMPILER: scripts/warning-count-baseline.txt has one row
# per compiler, "<key><TAB><C warning count>", where key is "gcc-<major>"
# read from `$CC -dumpversion`. This machine has gcc 16.2.1 and no gcc-14;
# CI runs `make CC=gcc-14 ...` (.github/workflows/tests.yml). A compiler
# with no row FAILS -- see evaluate() below -- it never silently passes.
#
# Usage:
#   scripts/warning_count_probe.sh                    # gate (exit 1 on red)
#   scripts/warning_count_probe.sh --update-baseline   # accept current count
#   scripts/warning_count_probe.sh --self-test         # prove it can go red
#
# Env (all optional, mainly for the Makefile target and the self-test):
#   CC_PROBE     -- compiler command to build and measure with (default gcc).
#                   May be two words ("<ccache> gcc") -- left UNQUOTED at
#                   every direct-invocation call site so the shell splits it,
#                   same convention as scripts/goo_assert_probe.sh.
#   CFLAGS_PROBE -- flags for the self-test's standalone single-file compile
#                   (default: the Makefile's own CFLAGS, read from disk).
set -u

PROBE="warning-count-probe"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BASELINE="${WARNING_COUNT_BASELINE:-$REPO_ROOT/scripts/warning-count-baseline.txt}"

SANDBOX_BUILDDIR="build/warning-count"
SANDBOX_BINDIR="$SANDBOX_BUILDDIR/bin"
SANDBOX_LIBDIR="$SANDBOX_BUILDDIR/lib"
LOGFILE="$REPO_ROOT/$SANDBOX_BUILDDIR/build.log"

CC_="${CC_PROBE:-gcc}"

# ---------------------------------------------------------------------------
# Concurrency guard: every mode below reads or writes the shared sandbox
# directory (build/warning-count) and its one build log, so two
# invocations of this script must never run at once -- for example a
# person running it by hand while `make verify-core` is also running it as
# part of VERIFY_ALL_DEPS. CONFIRMED by a direct test (2026-09-02): two
# overlapping runs corrupted the shared build log and produced a wrong
# C-warning count from the run that read it. flock SERIALIZES rather than
# refusing -- the second invocation just waits its turn on a lock file
# that sits next to, not inside, the sandbox directory it wipes each run.
# ---------------------------------------------------------------------------
mkdir -p "$REPO_ROOT/build"
exec 9>"$REPO_ROOT/build/.warning-count-probe.lock"
flock 9

# ---------------------------------------------------------------------------
# Compiler identity: KEY is what the baseline is keyed by ("gcc-16"); FULLVER
# is the human-readable version printed in the verdict ("16.2.1"). Left
# unquoted so a two-word CC_ ("<ccache> gcc") splits correctly.
# ---------------------------------------------------------------------------
GCC_VER="$($CC_ -dumpversion 2>/dev/null)"
GCC_FULLVER="$($CC_ -dumpfullversion 2>/dev/null)"
[ -n "$GCC_FULLVER" ] || GCC_FULLVER="$GCC_VER"
GCC_MAJOR="${GCC_VER%%.*}"
KEY="gcc-${GCC_MAJOR:-unknown}"

# The FULL CFLAGS value (flags, -I paths, -include headers), read from the
# Makefile itself so the self-test's standalone compile cannot drift out of
# sync with what actually built. FLAGS is the short -W/-std subset of the
# same line, for the verdict line only -- it is not enough on its own to
# compile anything (no -I, no -include), so the self-test must use the full
# value below, never FLAGS.
FULL_CFLAGS_LINE="$(grep -E '^CFLAGS[[:space:]]*=' "$REPO_ROOT/Makefile" | head -n1 | sed 's/^CFLAGS[[:space:]]*=[[:space:]]*//')"
# CFLAGS names $(WERROR) since Task 22. This probe COUNTS warnings, so it must
# see them as warnings: drop the make variable from the standalone line here, and
# pass WERROR= to the sandboxed make below.
FULL_CFLAGS_LINE="${FULL_CFLAGS_LINE//\$(WERROR)/}"
FLAGS="$(printf '%s\n' "$FULL_CFLAGS_LINE" | grep -oE -- '-Wall|-Wextra|-Wpedantic|-std=[A-Za-z0-9]+' | tr '\n' ' ' | sed 's/ $//')"
[ -n "$FLAGS" ] || FLAGS="-Wall -Wextra -std=c23"
[ -n "$FULL_CFLAGS_LINE" ] || FULL_CFLAGS_LINE="-Wall -Wextra -std=c23 -g -I. -Iinclude -D_GNU_SOURCE -include include/xalloc.h -include include/goo_assert.h"

# hash_of: sha256 of a file, or an empty string if it does not exist. Used
# to prove the shared-binary and real-parser-file invariants below by
# content, not by modification time -- a rebuild fast enough to land
# inside one filesystem mtime tick could otherwise go unnoticed.
hash_of() {
	sha256sum "$1" 2>/dev/null | awk '{print $1}'
}

# ---------------------------------------------------------------------------
# count_log: the counting rule, factored so the gate and --self-test measure
# the exact same thing against the exact same log file(s). Prints
# "<C count> <bison count>" on stdout.
# ---------------------------------------------------------------------------
count_log() {
	local filtered total bison
	filtered="$(grep -h "warning:" "$@" 2>/dev/null | grep -vE '^(g)?make(\[[0-9]+\])?: warning:')"
	total="$(printf '%s\n' "$filtered" | grep -c .)"
	bison="$(printf '%s\n' "$filtered" | grep -cE '\[-Wother\]|\[-Wconflicts-sr\]')"
	printf '%d %d\n' "$((total - bison))" "$bison"
}

# print_breakdown: one line per [-Wxxx] tag (or "(no -W flag)" for an
# untagged warning like macro redefinition), sorted by count descending, so
# task 22 can read off which warnings to remove first without re-deriving
# this from the raw log.
print_breakdown() {
	local filtered
	filtered="$(grep -h "warning:" "$@" 2>/dev/null | grep -vE '^(g)?make(\[[0-9]+\])?: warning:')"
	printf '%s\n' "$filtered" | awk '
		{
			if (match($0, /\[-W[a-zA-Z0-9=-]*\]/)) {
				tag = substr($0, RSTART, RLENGTH)
			} else {
				tag = "(no -W flag)"
			}
			count[tag]++
		}
		END { for (t in count) printf "%d\t%s\n", count[t], t }
	' | sort -rn -k1,1 | while IFS="$(printf '\t')" read -r n tag; do
		[ -n "${n:-}" ] || continue
		echo "$PROBE: $n $tag"
	done
}

# evaluate: shrink-only compare of a C count against the baseline row for
# $KEY. Sets EVAL_MSG and EVAL_BASE; returns 0 (green) or 1 (red/unknown).
evaluate() {
	local c="$1" base found
	found="$(awk -F'\t' -v k="$KEY" '$1==k{print "1"; exit}' "$BASELINE" 2>/dev/null)"
	if [ "$found" != "1" ]; then
		EVAL_MSG="no baseline for $KEY -- measured $c C warnings; run --update-baseline to add it"
		EVAL_BASE=""
		return 1
	fi
	base="$(awk -F'\t' -v k="$KEY" '$1==k{print $2; exit}' "$BASELINE")"
	EVAL_BASE="$base"
	if ! case "$base" in ''|*[!0-9]*) false ;; *) true ;; esac; then
		EVAL_MSG="baseline row for $KEY is not a whole number: '$base'"
		return 1
	fi
	if [ "$c" -gt "$base" ]; then
		EVAL_MSG="C warning count rose above baseline: $c > $base"
		return 1
	fi
	EVAL_MSG="within baseline ($c <= $base)"
	return 0
}

# ---------------------------------------------------------------------------
# --update-baseline: accept the current count for the CURRENT compiler's
# key. Never invoked by the gate itself. Preserves every other compiler's
# row untouched (this is how CI's gcc-14 row gets added later without
# disturbing gcc-16's row written here).
# ---------------------------------------------------------------------------
run_rebuild() { # sets C_COUNT, BUILD_RC; writes $LOGFILE
	local jobs real_bin real_lib before_bin before_lib after_bin after_lib sandbox_abs
	jobs="$(nproc 2>/dev/null || echo 4)"
	sandbox_abs="$REPO_ROOT/$SANDBOX_BUILDDIR"
	real_bin="$REPO_ROOT/bin/goo"
	real_lib="$REPO_ROOT/lib/libgoo_runtime.a"
	before_bin="$(hash_of "$real_bin")"
	before_lib="$(hash_of "$real_lib")"

	# NO -B: wipe the sandbox instead, so every sandboxed object rebuilds
	# from nothing without forcing anything OUTSIDE the sandbox (the
	# vendored NNG library) to rebuild too. See "NO -B" in the header.
	rm -rf "$sandbox_abs"
	mkdir -p "$sandbox_abs"
	( cd "$REPO_ROOT" && make -j"$jobs" \
		BUILDDIR="$SANDBOX_BUILDDIR" BINDIR="$SANDBOX_BINDIR" LIBDIR="$SANDBOX_LIBDIR" \
		WERROR= CC="$CC_" lexer ) >"$LOGFILE" 2>&1
	BUILD_RC=$?

	after_bin="$(hash_of "$real_bin")"
	after_lib="$(hash_of "$real_lib")"
	if [ "$before_bin" != "$after_bin" ] || [ "$before_lib" != "$after_lib" ]; then
		echo "$PROBE: FAIL (the sandboxed rebuild touched the shared compiler -- bin/goo sha256 ${before_bin:-MISSING} -> ${after_bin:-MISSING}, lib/libgoo_runtime.a sha256 ${before_lib:-MISSING} -> ${after_lib:-MISSING})"
		exit 1
	fi

	if [ "$BUILD_RC" -ne 0 ]; then
		echo "$PROBE: FAIL (the sandboxed rebuild exited $BUILD_RC -- see $LOGFILE)"
		tail -n 30 "$LOGFILE"
		exit 1
	fi

	read -r C_COUNT _ <<<"$(count_log "$LOGFILE")"
}

# run_bison_probe: the bison warning count, from a SECOND, dedicated bison
# run whose output goes only into the sandbox (never into src/). Sets
# BISON_COUNT and BISON_LOG. This is informational only -- see BISON COUNT
# in the header for why the main build log cannot supply this number
# anymore, now that the gate no longer forces bison to regenerate the real
# src/parser/parser.tab.c/.h on every run.
run_bison_probe() {
	local bison_dir real_c real_h before_c before_h after_c after_h
	bison_dir="$REPO_ROOT/$SANDBOX_BUILDDIR/bison-check"
	real_c="$REPO_ROOT/src/parser/parser.tab.c"
	real_h="$REPO_ROOT/src/parser/parser.tab.h"

	before_c="$(hash_of "$real_c")"
	before_h="$(hash_of "$real_h")"

	rm -rf "$bison_dir"
	mkdir -p "$bison_dir"
	BISON_LOG="$bison_dir/bison.log"
	( cd "$REPO_ROOT" && bison -d -o "$bison_dir/parser.tab.c" src/parser/parser.y ) 2>"$BISON_LOG"

	after_c="$(hash_of "$real_c")"
	after_h="$(hash_of "$real_h")"
	if [ "$before_c" != "$after_c" ] || [ "$before_h" != "$after_h" ]; then
		echo "$PROBE: FAIL (the dedicated bison count run touched the real src/parser/parser.tab.c or .h -- it must only ever write inside $bison_dir)"
		exit 1
	fi

	BISON_COUNT="$(grep -cE '\[-Wother\]|\[-Wconflicts-sr\]' "$BISON_LOG" 2>/dev/null)"
	[ -n "$BISON_COUNT" ] || BISON_COUNT=0
}

if [ "${1:-}" = "--update-baseline" ]; then
	run_rebuild
	tmp="$(mktemp)"
	if [ -f "$BASELINE" ]; then
		awk -F'\t' -v k="$KEY" -v v="$C_COUNT" 'BEGIN{OFS="\t"; done=0}
			/^#/ {print; next}
			/^[[:space:]]*$/ {next}
			$1==k {print k, v; done=1; next}
			{print}
			END { if (!done) print k, v }
		' "$BASELINE" >"$tmp"
	else
		{
			echo "# warning-count gate baseline. One row per compiler:"
			echo "#   <key><TAB><C warning count>"
			echo "#"
			echo "# key = gcc-<major version>, read from \`\$CC -dumpversion\`. This is NOT"
			echo "# the same as the bison/conflict count -- see the header of"
			echo "# scripts/warning_count_probe.sh for why that is a separate, ungated number."
			echo "#"
			echo "# Regenerate/add a row with:"
			echo "#   scripts/warning_count_probe.sh --update-baseline"
			echo "#"
			echo "# Shrink-only: the gate is red when the CURRENT C warning count for a key"
			echo "# EXCEEDS its row. A compiler with no row here FAILS the gate outright --"
			echo "# it never passes on an unknown compiler. The gate never writes this file"
			echo "# itself; only this flag does."
		} >"$tmp"
		printf '%s\t%s\n' "$KEY" "$C_COUNT" >>"$tmp"
	fi
	mv "$tmp" "$BASELINE"
	echo "$PROBE: baseline updated -- $KEY -> $C_COUNT C warnings ($BASELINE)"
	exit 0
fi

# ---------------------------------------------------------------------------
# --self-test: prove the gate can go red. Reuses the REAL build log from a
# gate run that must already have happened (the Makefile target runs the
# plain gate, then this, in that order -- see the warning-count-probe
# target) rather than paying for a second full rebuild. The only build this
# mode performs is compiling ONE small, mutated file, alone, to /dev/null.
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--self-test" ]; then
	SELF="$PROBE --self-test"

	if [ ! -f "$LOGFILE" ]; then
		echo "$SELF: FAIL (no build log at $LOGFILE -- run scripts/warning_count_probe.sh first)"
		exit 1
	fi

	W="$(mktemp -d)"
	trap 'rm -rf "$W"' EXIT
	bad_self=0

	# 1. Injection: the smallest *.c file under src/ (generated files
	#    excluded, same list rule as assertion_density_probe.sh), copied,
	#    with one unused local variable added right after the first block
	#    it finds open ("...) {" at end of line -- a function header or any
	#    control-flow header both work, since C allows a declaration at the
	#    top of ANY block).
	small_file="$(find "$REPO_ROOT/src" -type f -name '*.c' ! -name '*.tab.c' ! -name 'lex.yy.c' -exec wc -l {} \; 2>/dev/null | sort -n | head -n1 | awk '{print $2}')"
	if [ -z "$small_file" ]; then
		echo "$SELF: FAIL (found no src/*.c file to inject into)"
		rm -rf "$W"
		exit 1
	fi
	cp "$small_file" "$W/probe.c"
	inject_line="$(grep -nE '\)[[:space:]]*\{[[:space:]]*$' "$W/probe.c" | head -n1 | cut -d: -f1)"
	if [ -z "$inject_line" ]; then
		echo "$SELF: FAIL (no '...) {' line in $small_file to inject an unused variable after)"
		rm -rf "$W"
		exit 1
	fi
	sed -i "${inject_line}a\\    int _warning_count_probe_injected_unused = 0;" "$W/probe.c"

	cflags="${CFLAGS_PROBE:-$FULL_CFLAGS_LINE}"
	injected_stderr="$W/injected.stderr"
	( cd "$REPO_ROOT" && $CC_ $cflags -c "$W/probe.c" -o /dev/null ) 2>"$injected_stderr"

	if ! grep -q -- '\[-Wunused-variable\]' "$injected_stderr"; then
		echo "$SELF: FAIL (the injected unused variable did not trigger -Wunused-variable in $small_file -- injection did not land)"
		sed 's/^/        /' "$injected_stderr"
		rm -rf "$W"
		exit 1
	fi
	echo "    ok: the injected copy of $(basename "$small_file") shows -Wunused-variable"

	# 2. Real log + injected stderr must be RED.
	read -r combined_c combined_bison <<<"$(count_log "$LOGFILE" "$injected_stderr")"
	if evaluate "$combined_c"; then
		echo "$SELF: FAIL (real log + one injected unused variable stayed GREEN: $EVAL_MSG)"
		bad_self=1
	else
		echo "    ok: real log + one injected unused variable is RED ($EVAL_MSG)"
	fi

	# 3. Real log alone must be GREEN (the positive control -- without it, a
	#    harness that is red for every input would look identical to a
	#    working negative control).
	read -r real_c real_bison <<<"$(count_log "$LOGFILE")"
	if evaluate "$real_c"; then
		echo "    ok: the real log alone is GREEN ($EVAL_MSG)"
	else
		echo "$SELF: FAIL (the real log alone is RED: $EVAL_MSG)"
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

if [ -n "${1:-}" ]; then
	echo "usage: $0 [--update-baseline|--self-test]" >&2
	exit 2
fi

# ---------------------------------------------------------------------------
# The gate.
# ---------------------------------------------------------------------------
run_rebuild
run_bison_probe
print_breakdown "$LOGFILE" "$BISON_LOG"

if evaluate "$C_COUNT"; then
	printf '%s: PASS (%d C warnings, %d bison warnings, %s %s, %s, full rebuild of bin/goo, baseline %s)\n' \
		"$PROBE" "$C_COUNT" "$BISON_COUNT" "$KEY" "$GCC_FULLVER" "$FLAGS" "$EVAL_BASE"
	exit 0
else
	printf '%s: FAIL (%d C warnings, %d bison warnings, %s %s, %s, full rebuild of bin/goo, %s)\n' \
		"$PROBE" "$C_COUNT" "$BISON_COUNT" "$KEY" "$GCC_FULLVER" "$FLAGS" "$EVAL_MSG"
	exit 1
fi
