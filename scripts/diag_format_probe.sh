#!/usr/bin/env bash
# Gate for the compiler diagnostic format.
#
# Every user-facing error prints
#     <file>:<line>:<col>: <severity>: <message>      (or `goo: <severity>: <message>`)
#     %5d | <source line>
#           | <pad>^
#   [  help: <paste-ready fix>]
# This gate holds two sets of fixtures under tests/diag_format:
#
#   format/  NAME.goo (or NAME.cmd: the `goo build` arguments, "@OUT@" is the
#            output path) plus NAME.expect, the exact stderr. Covers a parse
#            error, a type error, a codegen error, a no-position error, a tab
#            line and a multi-byte line. Each stderr is also checked against
#            the header grammar, so a stale NAME.expect cannot bless an old
#            format.
#   help/    NAME.goo must fail WITH a help block. NAME.help holds that block
#            exactly as printed. NAME.fixed.goo (the help applied) must build
#            and must contain each code line of the help: the first help line,
#            and the text after "for example: " on a later line.
#            NAME.nohelp inverts this: NAME.goo must fail with NO help line,
#            and line 1 of NAME.nohelp is the error text it must print, so a
#            fixture that fails for another reason is refused.
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
FIXTURES="$ROOT/tests/diag_format"

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

# help_code_lines BLOCK_FILE: prints the paste-ready code of a help block, one
# per line, without the "  help: " lead. By convention the first help line is
# code. A later line is prose, except the text after "for example: ", which is
# code too.
help_code_lines() {
	local n=0 line text
	while IFS= read -r line; do
		n=$((n + 1))
		text=${line:8}
		if [ "$n" -eq 1 ]; then
			printf '%s\n' "$text"
		else
			case $text in *"for example: "*) printf '%s\n' "${text##*for example: }" ;; esac
		fi
	done < "$1"
}

# check_help_fixture NAME DIR TMP: the help of a failing fixture. The printed
# block must equal NAME.help, and the code in it must appear, line for line,
# in NAME.fixed.goo, which must build. So the proof file is the help applied.
check_help_fixture() {
	local name=$1 dir=$2 tmp=$3 code
	sed -n '/^  help: /,$p' "$tmp/herr" > "$tmp/hblock"
	if [ ! -s "$tmp/hblock" ]; then
		fail no-help "$name: no help line in stderr"
		return
	fi
	if [ ! -f "$dir/$name.help" ]; then
		fail no-help-file "$name: no $name.help with the expected help"
	elif ! cmp -s "$tmp/hblock" "$dir/$name.help"; then
		fail help-differs "$name: printed help differs from $name.help:"
		cat "$tmp/hblock"
	fi
	if [ ! -f "$dir/$name.fixed.goo" ]; then
		fail no-fixed "$name: no .fixed.goo"
		return
	fi
	sed 's/^[[:space:]]*//;s/[[:space:]]*$//' "$dir/$name.fixed.goo" > "$tmp/fixed.trim"
	while IFS= read -r code; do
		grep -Fxq -- "$code" "$tmp/fixed.trim" ||
			fail help-not-in-fixed "$name: help code '$code' is not a line of $name.fixed.goo"
	done < <(help_code_lines "$tmp/hblock")
	if ! "$COMPILER" build -o "$tmp/ok" "$dir/$name.fixed.goo" 2>"$tmp/herr2"; then
		fail fixed-build "$name: the fixed fixture does not build"
		cat "$tmp/herr2"
	fi
}

# check_nohelp_fixture NAME DIR TMP: NAME.nohelp line 1 is the error message
# that the fixture must print. Any other failure is refused, so a fixture that
# breaks for another reason cannot pass as "no help".
check_nohelp_fixture() {
	local name=$1 dir=$2 tmp=$3 want
	want=$(head -n 1 "$dir/$name.nohelp")
	if [ -z "$want" ]; then
		fail nohelp-no-message "$name: $name.nohelp line 1 must hold the expected error text"
	elif ! head -n 1 "$tmp/herr" | grep -Fq -- "$want"; then
		fail nohelp-error "$name: first stderr line lacks '$want': $(head -n 1 "$tmp/herr")"
	fi
	if grep -q '^  help: ' "$tmp/herr"; then
		fail help-forbidden "$name: help line where none is allowed"
	fi
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
			check_nohelp_fixture "$name" "$dir" "$tmp"
		else
			check_help_fixture "$name" "$dir" "$tmp"
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

	# Help mutants, using the real compiler on copies of repo fixtures.
	local h="$tmp/help" src="$FIXTURES/help"
	mkdir -p "$h"
	help_case() { ( FAILS=0; run_help_dir "$h" "$tmp" ) > "$CAPTURE"; }
	reset_help() {
		rm -rf "$h"; mkdir -p "$h"
		cp "$src/error_union.goo" "$h/bad.goo"
		cp "$src/error_union.fixed.goo" "$h/bad.fixed.goo"
		cp "$src/error_union.help" "$h/bad.help"
	}

	reset_help
	help_case
	if grep -q '^FAIL' "$CAPTURE"; then
		echo "SELF-TEST FAIL: the unchanged help control was refused"; SELF_FAILED=1
		cat "$CAPTURE"
	else
		echo "self-test: help control -> GREEN (ok)"
	fi

	reset_help
	{ cat "$src/error_union.fixed.goo"; echo 'this is not goo'; } > "$h/bad.fixed.goo"
	help_case; expect_red fixed-build "a .fixed.goo that has the help code but does not build"

	reset_help
	sed 's/x, err := f()/x, e := f()/; s/_ = err/_ = e/' "$src/error_union.fixed.goo" > "$h/bad.fixed.goo"
	help_case; expect_red help-not-in-fixed "a .fixed.goo that builds but lacks the help code"

	reset_help
	printf '  help: x, err := g()\n' > "$h/bad.help"
	help_case; expect_red help-differs "printed help differs from NAME.help"

	reset_help
	rm "$h/bad.help"
	help_case; expect_red no-help-file "a help fixture without NAME.help"

	reset_help
	cp "$src/error_union_chain.goo" "$h/bad.goo"; rm -f "$h/bad.fixed.goo" "$h/bad.help"
	printf 'error union must be handled\n' > "$h/bad.nohelp"
	help_case
	if grep -q '^FAIL' "$CAPTURE"; then
		echo "SELF-TEST FAIL: the unchanged nohelp control was refused"; SELF_FAILED=1
		cat "$CAPTURE"
	else
		echo "self-test: nohelp control -> GREEN (ok)"
	fi

	printf 'a message that is not printed\n' > "$h/bad.nohelp"
	help_case; expect_red nohelp-error "a .nohelp fixture that fails with a different error"

	: > "$h/bad.nohelp"
	help_case; expect_red nohelp-no-message "a .nohelp fixture with no expected message"

	reset_help
	rm -f "$h/bad.fixed.goo" "$h/bad.help"
	printf 'error union must be handled\n' > "$h/bad.nohelp"
	help_case; expect_red help-forbidden "a .nohelp fixture that prints help"

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
