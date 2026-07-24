#!/bin/bash
# ADR 0001 nil-deref probes: each unguarded-site fixture must PANIC with
# Go's canonical message and exit 2 — never SIGSEGV (exit 139). Mirrors
# scripts/exit_code_probe.sh's inline-fixture harness. Also pins the
# LEGAL nil cases (typed-nil method dispatch without field access) so the
# checks never over-fire and break Go parity.

set -u

fail() { echo "FAIL: $1"; exit 1; }

COMPILER="$(cd "$(dirname "$0")/.." && pwd)/bin/goo"
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

[ -x "$COMPILER" ] || fail "compiler not found at $COMPILER (run 'make')"

MSG='panic: runtime error: invalid memory address or nil pointer dereference'

# Compile $2, run it, assert exit 2 + both stderr markers. $1 = case name.
check_nilpanic() {
    local name="$1"
    local src="$WORKDIR/$name.goo" exe="$WORKDIR/$name.out" err="$WORKDIR/$name.err"
    printf "%s" "$2" > "$src"
    if ! "$COMPILER" "$src" -o "$exe" > "$WORKDIR/$name.log" 2>&1; then
        sed 's/^/    /' "$WORKDIR/$name.log"; fail "$name: compilation failed"
    fi
    "$exe" >/dev/null 2>"$err"; local got=$?
    [ "$got" = "2" ] || { sed 's/^/    stderr: /' "$err"; fail "$name: exit $got, expected 2 (139 = still SIGSEGV)"; }
    grep -q "nil dereference at " "$err" || { sed 's/^/    stderr: /' "$err"; fail "$name: missing file:line diagnostic"; }
    grep -qF "$MSG" "$err" || { sed 's/^/    stderr: /' "$err"; fail "$name: missing canonical panic message"; }
    echo "  ok: $name (panic, exit 2)"
}

# Compile $2, run it, assert clean exit 0 and stdout == $3. $1 = case name.
check_ok() {
    local name="$1" expected_out="$3"
    local src="$WORKDIR/$name.goo" exe="$WORKDIR/$name.out"
    printf "%s" "$2" > "$src"
    if ! "$COMPILER" "$src" -o "$exe" > "$WORKDIR/$name.log" 2>&1; then
        sed 's/^/    /' "$WORKDIR/$name.log"; fail "$name: compilation failed"
    fi
    local out; out="$("$exe" 2>"$WORKDIR/$name.err")"; local got=$?
    [ "$got" = "0" ] || { sed 's/^/    stderr: /' "$WORKDIR/$name.err"; fail "$name: exit $got, expected 0"; }
    [ "$out" = "$expected_out" ] || fail "$name: stdout '$out', expected '$expected_out'"
    echo "  ok: $name (legal, exit 0)"
}

# Like check_nilpanic, but also asserts stdout == $3 instead of discarding
# it (T3 review Important finding #2). Pins Go's evaluation order: a call's
# ARGUMENTS (including their side effects) are evaluated before a
# nil-interface dispatch panics — so a printing arg expression must still
# be visible on stdout even though the call itself panics.
check_nilpanic_stdout() {
    local name="$1" expected_out="$3"
    local src="$WORKDIR/$name.goo" exe="$WORKDIR/$name.out" err="$WORKDIR/$name.err"
    printf "%s" "$2" > "$src"
    if ! "$COMPILER" "$src" -o "$exe" > "$WORKDIR/$name.log" 2>&1; then
        sed 's/^/    /' "$WORKDIR/$name.log"; fail "$name: compilation failed"
    fi
    local out; out="$("$exe" 2>"$err")"; local got=$?
    [ "$got" = "2" ] || { sed 's/^/    stderr: /' "$err"; fail "$name: exit $got, expected 2 (139 = still SIGSEGV)"; }
    [ "$out" = "$expected_out" ] || fail "$name: stdout '$out', expected '$expected_out' (argument side effect discarded — wrong evaluation order?)"
    grep -q "nil dereference at " "$err" || { sed 's/^/    stderr: /' "$err"; fail "$name: missing file:line diagnostic"; }
    grep -qF "$MSG" "$err" || { sed 's/^/    stderr: /' "$err"; fail "$name: missing canonical panic message"; }
    echo "  ok: $name (panic, exit 2, stdout preserved)"
}

check_nilpanic star_read 'package main
import "fmt"
func main() {
	var p *int
	fmt.Println(*p)
}
'

check_nilpanic star_write 'package main
func main() {
	var p *int
	*p = 1
}
'

check_nilpanic field_read 'package main
import "fmt"
type T struct{ x int }
func main() {
	var p *T
	fmt.Println(p.x)
}
'

check_nilpanic field_write 'package main
type T struct{ x int }
func main() {
	var p *T
	p.x = 1
}
'

check_nilpanic nil_receiver_method_field 'package main
import "fmt"
type T struct{ x int }
func (t *T) Get() int { return t.x }
func main() {
	var p *T
	fmt.Println(p.Get())
}
'

# LEGAL Go: a method on a typed-nil receiver that never touches fields
# runs fine — the check must live at the FIELD access, not the call.
check_ok nil_receiver_method_no_field 'package main
import "fmt"
type T struct{ x int }
func (t *T) Tag() int { return 42 }
func main() {
	var p *T
	fmt.Println(p.Tag())
}
' '42'

# LEGAL: non-nil paths unaffected.
check_ok non_nil_paths 'package main
import "fmt"
type T struct{ x int }
func main() {
	v := 7
	p := &v
	*p = *p + 1
	t := &T{x: 5}
	t.x = t.x + 1
	fmt.Println(*p)
	fmt.Println(t.x)
}
' '8
6'

check_nilpanic nil_interface_dispatch 'package main
import "fmt"
type Speaker interface{ Speak() int }
func main() {
	var s Speaker
	fmt.Println(s.Speak())
}
'

# LEGAL Go: an interface HOLDING a typed-nil *T dispatches fine; only a
# field access inside the method panics (covered by Task 2 site C).
check_ok typed_nil_in_interface 'package main
import "fmt"
type Speaker interface{ Speak() int }
type T struct{ x int }
func (t *T) Speak() int { return 9 }
func main() {
	var p *T
	var s Speaker = p
	fmt.Println(s.Speak())
}
' '9'

# T3 review Important finding #2: the call's argument (sideEffect(), which
# prints "arg") must run BEFORE the nil-interface dispatch panics — Go
# evaluates call arguments before the call, and the interface-nil panic
# happens as part of the call itself. fmt.Println's own argument
# (s.Speak(...)) never gets far enough to run Println, so stdout must be
# exactly "arg" from sideEffect(), nothing more.
check_nilpanic_stdout nil_interface_dispatch_arg_order 'package main
import "fmt"
type Speaker interface{ Speak(x int) int }
func sideEffect() int {
	fmt.Println("arg")
	return 1
}
func main() {
	var s Speaker
	fmt.Println(s.Speak(sideEffect()))
}
' 'arg'

check_nilpanic error_nil_error 'package main
import "fmt"
func main() {
	var e error
	fmt.Println(e.Error())
}
'

echo "nil-deref-probe: PASS (all cases)"
