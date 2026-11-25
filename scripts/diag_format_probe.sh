#!/usr/bin/env bash
# Gate for the compiler diagnostic format.
#
# Every user-facing error prints
#     <file>:<line>:<col>: <severity>: <message>      (or `goo: <severity>: <message>`)
#     %5d | <source line>
#           | <pad>^
#   [  help: <paste-ready fix>]
# This gate holds two sets of fixtures under tests/probes/src/diag-format-probe:
#
#   format/  NAME.goo (or NAME.cmd: the `goo build` arguments, "@OUT@" is the
#            output path) plus NAME.expect, the exact stderr. Covers a parse
#            error, a type error, a codegen error, a no-position error, a tab
#            line and a multi-byte line. Each stderr is also checked against
#            the header grammar, so a stale NAME.expect cannot bless an old
#            format.
#   help/    NAME.goo must fail WITH a "  help: " line and NAME.fixed.goo (the
#            help applied) must build. A NAME.nohelp marker inverts the first
#            half: NAME.goo must fail with NO help line.
#
# Excluded on purpose: lexer errors have no snippet (lexer.c is compiled by
# name in several probes) and the in-memory _testmain file has none.
#
# A gate that finds zero fixtures is broken, not green: both sets fail on an
# empty directory. Exit codes are read directly, never through a pipe.
# Usage: diag_format_probe.sh [--self-test]
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
COMPILER="${COMPILER:-$ROOT/bin/goo}"
case "$COMPILER" in /*) ;; *) COMPILER="$PWD/$COMPILER" ;; esac
FIXTURES="$ROOT/tests/probes/src/diag-format-probe"

FAILS=0
# fail TAG MESSAGE: the TAG is what the self-test matches, so each mutant must
# be refused for its own reason and not for any reason at all.
fail() { echo "FAIL[$1]: $2"; FAILS=$((FAILS + 1)); }

HEADER_RE='^[^ :][^:]*:[0-9]+:[0-9]+: (error|warning): .+$|^goo: (error|warning): .+$'

# check_stderr NAME EXPECT_FILE STDERR_FILE
check_stderr() {
	local name=$1 expect=$2 got=$3 line before=$FAILS
	line=$(head -n 1 "$got")
	if ! printf '%s\n' "$line" | grep -Eq "$HEADER_RE"; then
		fail header "$name: first line is not '<file>:<l>:<c>: <severity>: <msg>': $line"
		return
	fi
	if [ "$(sed -n 2p "$got")" != "$(sed -n 2p "$expect")" ]; then
		fail snippet "$name: snippet line differs: $(sed -n 2p "$got")"
	fi
	if [ "$(sed -n 3p "$got")" != "$(sed -n 3p "$expect")" ]; then
		fail caret "$name: caret line differs: $(sed -n 3p "$got")"
	fi
	# Lines 1-3 were checked above, so a difference here is in the rest.
	if [ "$FAILS" -eq "$before" ] && ! cmp -s "$got" "$expect"; then
		fail stderr "$name: stderr differs from $(basename "$expect")"
	fi
}

# run_format_dir DIR TMP: returns after counting failures into FAILS.
run_format_dir() {
	local dir=$1 tmp=$2 n=0 f name args out before
	for f in "$dir"/*.expect; do
		[ -e "$f" ] || break
		name=$(basename "$f" .expect)
		n=$((n + 1))
		if [ -f "$dir/$name.cmd" ]; then
			args=$(sed "s#@OUT@#$tmp/out#" "$dir/$name.cmd")
		else
			args="-o $tmp/out $name.goo"
		fi
		# shellcheck disable=SC2086
		(cd "$dir" && "$COMPILER" build $args >/dev/null 2>"$tmp/err")
		out=$?
		if [ "$out" -eq 0 ]; then
			fail compiled "$name: the fixture compiled but must fail"
			continue
		fi
		before=$FAILS
		check_stderr "$name" "$f" "$tmp/err"
		[ "$FAILS" -eq "$before" ] || { echo "--- actual stderr of $name:"; cat "$tmp/err"; }
	done
	[ "$n" -gt 0 ] || fail no-fixtures "no *.expect fixtures in $dir"
	FORMAT_N=$n
}

# run_help_dir DIR TMP
run_help_dir() {
	local dir=$1 tmp=$2 n=0 f name
	for f in "$dir"/*.goo; do
		[ -e "$f" ] || break
		case $f in *.fixed.goo) continue ;; esac
		name=$(basename "$f" .goo)
		n=$((n + 1))
		if "$COMPILER" build -o "$tmp/bad" "$f" 2>"$tmp/herr"; then
			fail compiled "$name: the help fixture compiled but must fail"
			continue
		fi
		if [ -f "$dir/$name.nohelp" ]; then
			if grep -q '^  help: ' "$tmp/herr"; then
				fail help-forbidden "$name: help line where none is allowed"
			fi
			continue
		fi
		grep -q '^  help: ' "$tmp/herr" || fail no-help "$name: no help line in stderr"
		if [ ! -f "$dir/$name.fixed.goo" ]; then
			fail no-fixed "$name: no .fixed.goo"
		elif ! "$COMPILER" build -o "$tmp/ok" "$dir/$name.fixed.goo" 2>"$tmp/herr2"; then
			fail fixed-build "$name: the fixed fixture does not build"
			cat "$tmp/herr2"
		fi
	done
	[ "$n" -gt 0 ] || fail no-fixtures "no help fixtures in $dir"
	HELP_N=$n
}

probe() {
	local tmp
	tmp=$(mktemp -d) || return 1
	run_format_dir "$FIXTURES/format" "$tmp"
	run_help_dir "$FIXTURES/help" "$tmp"
	rm -rf "$tmp"
	if [ "$FAILS" -ne 0 ]; then
		echo "diag-format-probe: FAIL ($FAILS problems)"
		return 1
	fi
	echo "diag-format-probe: PASS ($FORMAT_N format fixtures, $HELP_N help fixtures)"
}

# expect_red TAG: the last captured output must be red for TAG and no other
# tag, because a mutant refused for the wrong reason proves nothing.
SELF_FAILED=0
expect_red() {
	local tag=$1 label=$2 other
	if ! grep -q "^FAIL\[$tag\]" "$CAPTURE"; then
		echo "SELF-TEST FAIL: $label was not refused with [$tag]"; SELF_FAILED=1; return
	fi
	other=$(grep '^FAIL\[' "$CAPTURE" | grep -v "^FAIL\[$tag\]")
	if [ -n "$other" ]; then
		echo "SELF-TEST FAIL: $label also tripped another check: $other"; SELF_FAILED=1; return
	fi
	echo "self-test: $label -> RED [$tag] (ok)"
}

self_test() {
	local tmp t
	tmp=$(mktemp -d) || return 1
	CAPTURE="$tmp/capture"
	# The inputs are built here, from literals and from repo fixtures. Nothing
	# is read from PATH or the machine.
	t="$tmp/fmt"; mkdir -p "$t"
	printf 'x.goo:4:2: error: Cannot assign int64 to string\n    4 | \tvar s string = 5\n      | \t^\n' > "$t/good.err"
	cp "$t/good.err" "$t/expect"

	# check_stderr bumps FAILS in this shell, so each case runs in a subshell
	# whose output is the capture.
	one() { ( FAILS=0; check_stderr "$1" "$t/expect" "$2" ) > "$CAPTURE"; }

	one control "$t/good.err"
	if grep -q '^FAIL' "$CAPTURE"; then
		echo "SELF-TEST FAIL: the unchanged control was refused"; SELF_FAILED=1
		cat "$CAPTURE"
	else
		echo "self-test: control -> GREEN (ok)"
	fi

	sed '3s/\^/ ^/' "$t/good.err" > "$t/caret.err"
	one caret "$t/caret.err"; expect_red caret "caret moved one column"

	sed '1s/^x.goo:4:2: error: /Type error at /' "$t/good.err" > "$t/prefix.err"
	one prefix "$t/prefix.err"; expect_red header "old 'Type error at' prefix"

	sed '2s/var s/var z/' "$t/good.err" > "$t/snip.err"
	one snip "$t/snip.err"; expect_red snippet "wrong snippet line"

	printf 'x.goo:4:2: error: Cannot assign int64 to string\n    4 | \tvar s string = 5\n      | \t^\n  help: extra\n' > "$t/extra.err"
	one extra "$t/extra.err"; expect_red stderr "an extra trailing line"

	# Help mutants, using the real compiler on a copy of a repo fixture.
	local h="$tmp/help" src="$FIXTURES/help"
	mkdir -p "$h"
	cp "$src/error_union.goo" "$h/bad.goo"
	printf 'package main\nfunc main() { this is not goo\n' > "$h/bad.fixed.goo"
	( FAILS=0; run_help_dir "$h" "$tmp" ) > "$CAPTURE"
	expect_red fixed-build "a .fixed.goo that does not compile"

	rm -f "$h/bad.fixed.goo"; : > "$h/bad.nohelp"
	( FAILS=0; run_help_dir "$h" "$tmp" ) > "$CAPTURE"
	expect_red help-forbidden "a .nohelp fixture that prints help"

	rm -rf "$h"; mkdir -p "$h"
	( FAILS=0; run_help_dir "$h" "$tmp" ) > "$CAPTURE"
	expect_red no-fixtures "an empty help directory"

	( FAILS=0; run_format_dir "$h" "$tmp" ) > "$CAPTURE"
	expect_red no-fixtures "an empty format directory"

	rm -rf "$tmp"
	if [ "$SELF_FAILED" -ne 0 ]; then echo "diag-format-selftest: FAIL"; return 1; fi
	echo "diag-format-selftest: PASS"
}

case "${1:-}" in
	--self-test) self_test; exit $? ;;
	"") probe; exit $? ;;
	*) echo "usage: $0 [--self-test]" >&2; exit 2 ;;
esac
