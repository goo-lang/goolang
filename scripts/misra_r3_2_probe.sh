#!/usr/bin/env bash
# misra-r3-2-probe -- the MISRA C:2012 Rule 3.2 gate.
#
# Rule 3.2: "Line-splicing shall not be used in // comments". A // comment on
# a line whose LAST character is a backslash continues onto the next line, so
# the next line of code is silently commented out.
#
# WHY GREP. Task 12 (docs/misra/cobra-calibration.md) calibrated nine rules
# with Cobra. Only Rule 3.2 passed both controls, and it had 0 reports on
# src/. The user decided on 2026-11-08 to close the Cobra track and to gate
# this one rule with grep and no third-party dependency.
#
# THE BAR IS 0, SO THERE IS NO BASELINE FILE. src/ and include/ hold 0 cases
# on 2026-11-08, and the gate fails on ANY case. A baseline of 0 needs no file.
#
# THE RULE THIS COUNTS. A line that matches '//' followed by anything, then a
# backslash as the LAST character. A backslash followed by a space is not a
# splice in C (the compiler splices only backslash-newline), so the pattern
# anchors the backslash at end of line with no trailing-space allowance.
#
# KNOWN LIMIT, WHICH ERRS TOWARD A FALSE POSITIVE. The pattern does not know
# whether a '//' sits inside a string literal or inside a /* */ comment, so
# such a line that also ends in a backslash is reported. A false positive is
# loud (a human reads the line and rewrites it). A false negative would be
# silent, and the pattern has none that is known: every real splice in a //
# comment has a '//' before a final backslash. A CRLF file hides the final
# backslash behind a carriage return; the tree is LF.
#
# FILE LIST. `git ls-files` for *.c and *.h under src/ and include/. Never
# `find`: it returns the ignored bison output src/parser/parser.tab.c.
#
# Usage:
#   scripts/misra_r3_2_probe.sh              # gate (exit 1 on red)
#   scripts/misra_r3_2_probe.sh --self-test  # prove it can go red
set -u

PROBE="misra-r3-2-probe"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# MISRA_R3_2_ROOT overrides the tree that is read. The self-test points it at
# a scratch git repository.
ROOT="${MISRA_R3_2_ROOT:-$REPO_ROOT}"
PATTERN='//.*\\$'

# scan: print "FILES <n>" on the first line and then each hit as
# file:line:text. Return 0 for green, 1 for red, 2 when the instrument is
# broken (no git, or an empty file list).
scan() {
	local list files=() out rc
	if ! list="$(git -C "$ROOT" ls-files -- 'src/*.c' 'src/*.h' 'include/*.c' 'include/*.h' 2>&1)"; then
		echo "$PROBE: FAIL (git ls-files failed in $ROOT: $list)"
		return 2
	fi
	if [ -n "$list" ]; then
		mapfile -t files <<<"$list"
	fi
	if [ "${#files[@]}" -eq 0 ]; then
		echo "$PROBE: FAIL (git ls-files returned no C file in $ROOT -- the file list is broken, not the source)"
		return 2
	fi
	out="$(cd "$ROOT" && grep -n -H -E -- "$PATTERN" "${files[@]}")"
	rc=$?
	if [ "$rc" -gt 1 ]; then
		echo "$PROBE: FAIL (grep exited $rc)"
		return 2
	fi
	if [ "$rc" -eq 0 ]; then
		echo "$PROBE: FAIL ($(printf '%s\n' "$out" | grep -c .) line-splice in // comment, ${#files[@]} files read, list from git ls-files, baseline 0)"
		printf '%s\n' "$out" | sed 's/^/  /'
		return 1
	fi
	echo "$PROBE: PASS (0 line-splices in // comments, ${#files[@]} files read, list from git ls-files, baseline 0)"
	return 0
}

if [ "${1:-}" = "--self-test" ]; then
	SELF="$PROBE --self-test"
	W="$(mktemp -d)"
	trap 'rm -rf "$W"' EXIT
	bad=0
	# The target is the first listed file, so the injection lands in a file
	# the gate reads.
	TARGET_REL="$(git -C "$REPO_ROOT" ls-files -- 'src/*.c' | head -n1)"
	if [ -z "$TARGET_REL" ]; then
		echo "$SELF: FAIL (no src/*.c file to inject into)"
		exit 1
	fi

	mk() { # fresh scratch git repo holding a copy of the tracked sources
		rm -rf "$W/t"
		mkdir -p "$W/t"
		git -C "$REPO_ROOT" ls-files -z -- src include | (cd "$REPO_ROOT" && xargs -0 cp --parents -t "$W/t") || return 1
		git -C "$W/t" init -q && git -C "$W/t" add -A
	}
	run() { MISRA_R3_2_ROOT="$W/t" "$0" >"$W/out.log" 2>&1; }

	# (c) the unmodified copy -> GREEN
	mk || { echo "$SELF: FAIL (cannot build the scratch copy)"; exit 1; }
	if run; then
		echo "    ok (c): the unmodified scratch copy is GREEN: $(cat "$W/out.log")"
	else
		echo "$SELF: FAIL (c: the unmodified copy is RED)"; sed 's/^/        /' "$W/out.log"; bad=1
	fi

	# (a) one splice line, in the shape of tests/misra/cobra/r3_2_bad.c -> RED
	mk
	printf '%s\n' '// this comment ends in a splice \' >>"$W/t/$TARGET_REL"
	if ! grep -qF 'ends in a splice \' "$W/t/$TARGET_REL"; then
		echo "$SELF: FAIL (a: the injection did not land)"; bad=1
	elif run; then
		echo "$SELF: FAIL (a: stayed GREEN after one // line ending in a backslash)"; bad=1
	else
		echo "    ok (a): one // line ending in a backslash is RED: $(head -n1 "$W/out.log")"
	fi

	# (b) backslash then a space does not splice -> GREEN
	mk
	printf '%s\n' '// this comment ends in a backslash and a space \ ' >>"$W/t/$TARGET_REL"
	if ! grep -qF 'and a space \ ' "$W/t/$TARGET_REL"; then
		echo "$SELF: FAIL (b: the injection did not land)"; bad=1
	elif run; then
		echo "    ok (b): a backslash then a space is GREEN: $(cat "$W/out.log")"
	else
		echo "$SELF: FAIL (b: backslash and space went RED)"; sed 's/^/        /' "$W/out.log"; bad=1
	fi

	# (d) positive control: an empty file list must FAIL, not pass
	rm -rf "$W/t"; mkdir -p "$W/t"; git -C "$W/t" init -q
	if run; then
		echo "$SELF: FAIL (d: an empty file list passed)"; bad=1
	else
		echo "    ok (d): an empty file list is RED: $(head -n1 "$W/out.log")"
	fi

	rm -rf "$W"; trap - EXIT
	if [ "$bad" -ne 0 ]; then echo "$SELF: FAIL"; exit 1; fi
	echo "$SELF: PASS"
	exit 0
fi

if [ -n "${1:-}" ]; then
	echo "usage: $0 [--self-test]" >&2
	exit 2
fi

scan
