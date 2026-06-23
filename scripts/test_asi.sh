#!/bin/bash
# Automatic Semicolon Insertion test (roadmap Phase 1).
# Idiomatic Go source has no explicit statement semicolons. These fixtures must
# reach "Parsing complete" with zero hand-inserted semicolons. Run from repo root.

set -u

fail() { echo "FAIL: $1"; exit 1; }

COMPILER="$(cd "$(dirname "$0")/.." && pwd)/bin/goo"
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

[ -x "$COMPILER" ] || fail "compiler not found at $COMPILER (run 'make')"

# Assert a semicolon-free program reaches the parse AND type-check stages
# cleanly. Phase 1 owns parsing only: downstream codegen bugs (e.g. the P4-1
# return-statement crash) are out of scope, so we line-buffer the output and do
# not require a zero exit code -- only that the front end succeeded.
# $1 = case name, stdin = source.
check_parses() {
    local name="$1"
    local src="$WORKDIR/$name.goo"
    cat > "$src"
    if grep -q ';' "$src"; then
        fail "$name: fixture contains an explicit ';' (must be semicolon-free)"
    fi
    # Run in a child shell so an expected downstream codegen crash (P4-1) does
    # not print a "Segmentation fault" job message; correctness is judged purely
    # by the captured front-end output below.
    bash -c 'stdbuf -oL -eL "$0" "$1" > "$2" 2>&1' "$COMPILER" "$src" "$WORKDIR/$name.log" 2>/dev/null
    if grep -qi "Parse error" "$WORKDIR/$name.log" || ! grep -qi "Parsing complete" "$WORKDIR/$name.log"; then
        grep -iE "Parse error|Parsing" "$WORKDIR/$name.log" | sed 's/^/    /'
        fail "$name: did not reach 'Parsing complete' on semicolon-free source"
    fi
    if ! grep -qi "Type checking complete" "$WORKDIR/$name.log"; then
        fail "$name: parsed but did not reach 'Type checking complete'"
    fi
    echo "  ok: $name"
}

check_parses simple_decls <<'GOO'
package main

func main() {
    x := 5
    y := x + 1
    return
}
GOO

check_parses var_and_assign <<'GOO'
package main

func main() {
    var a int = 3
    a = a + 1
    return
}
GOO

# Multi-line expression inside parentheses: a line ending on a value while
# inside ( ) must NOT trigger ASI, or the expression is split mid-statement.
check_parses multiline_paren <<'GOO'
package main

func main() {
    x := (1 +
        2
    )
    return
}
GOO

# A block statement is terminated by ASI after its closing brace, so a
# following statement parses as a separate statement.
check_parses block_then_stmt <<'GOO'
package main

func main() {
    if true {
        x := 1
    }
    y := 2
    return
}
GOO

check_parses if_else <<'GOO'
package main

func main() {
    if true {
        x := 1
    } else {
        x := 2
    }
    return
}
GOO

check_parses nested_if <<'GOO'
package main

func main() {
    if true {
        if false {
            x := 1
        }
    }
    return
}
GOO

# A trailing line comment must not prevent the newline's semicolon insertion.
check_parses comment_eol <<'GOO'
package main

func main() {
    x := 5 // a trailing comment
    y := 6
    return
}
GOO

echo "PASS: semicolon-free source parses (ASI)"
exit 0
