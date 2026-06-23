#!/bin/bash
# main() exit-code test (M9). A Goo program is the C entry point: on normal
# completion it must exit 0, not return a garbage register value. Run from repo
# root after `make` + the runtime archive are built.

set -u

fail() { echo "FAIL: $1"; exit 1; }

COMPILER="$(cd "$(dirname "$0")/.." && pwd)/bin/goo"
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

[ -x "$COMPILER" ] || fail "compiler not found at $COMPILER (run 'make')"

# Compile $2, run it, assert exit code == $3. $1 = case name.
check_exit() {
    local name="$1" expected="$3"
    local src="$WORKDIR/$name.goo" exe="$WORKDIR/$name.out"
    printf "%s" "$2" > "$src"
    if ! "$COMPILER" "$src" -o "$exe" > "$WORKDIR/$name.log" 2>&1; then
        sed 's/^/    /' "$WORKDIR/$name.log"; fail "$name: compilation failed"
    fi
    "$exe" >/dev/null 2>&1; local got=$?
    [ "$got" = "$expected" ] || fail "$name: exit $got, expected $expected"
    echo "  ok: $name (exit $got)"
}

check_exit empty_main          'package main
func main() {}
' 0

check_exit stmts_no_return     'package main
func main() {
    x := 7
    y := x + 1
}
' 0

check_exit bare_return         'package main
func main() {
    x := 5
    return
}
' 0

echo "PASS: main() exits 0 on normal completion"
exit 0
