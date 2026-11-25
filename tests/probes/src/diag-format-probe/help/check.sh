#!/bin/sh
# Help-line check (Task 5; Task 6 absorbs it into the diag-format gate).
# For each NAME.goo: `goo build` must fail with a "  help: " line, and
# NAME.fixed.goo (the help applied) must build with exit 0. A NAME.nohelp
# marker file inverts this: NAME.goo must fail with NO help line, and needs
# no fixed file (no correct paste-ready fix exists there).
# Usage: check.sh [path-to-goo]   (default: bin/goo from the repo root)
here=$(cd "$(dirname "$0")" && pwd)
goo=${1:-$here/../../../../../bin/goo}
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
fail=0 n=0
for f in "$here"/*.goo; do
	case $f in *.fixed.goo) continue ;; esac
	name=$(basename "$f" .goo)
	n=$((n + 1))
	if "$goo" build -o "$tmp/bad" "$f" 2>"$tmp/err"; then
		echo "FAIL $name: the error fixture compiled"; fail=1
	elif [ -f "$here/$name.nohelp" ]; then
		if grep -q '^  help: ' "$tmp/err"; then
			echo "FAIL $name: help line where none is allowed"; fail=1
		fi
		continue
	elif ! grep -q '^  help: ' "$tmp/err"; then
		echo "FAIL $name: no help line in stderr"; fail=1
	fi
	if [ ! -f "$here/$name.fixed.goo" ]; then
		echo "FAIL $name: no .fixed.goo"; fail=1
	elif ! "$goo" build -o "$tmp/ok" "$here/$name.fixed.goo" 2>"$tmp/err2"; then
		echo "FAIL $name: fixed fixture does not build"; cat "$tmp/err2"; fail=1
	fi
done
[ "$n" -gt 0 ] || { echo "FAIL: no fixtures found"; exit 1; }
[ "$fail" -eq 0 ] && echo "help-check: PASS ($n fixtures)"
exit $fail
