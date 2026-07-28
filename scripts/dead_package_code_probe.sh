#!/bin/bash
# dead-package-code probe: an imported package's UNUSED functions must not
# survive into the emitted IR.
#
# Why this exists. Imported-package functions were emitted with EXTERNAL
# linkage, so LLVM's globaldce could not remove them however unreachable they
# were. A hello-world calling `strings.Repeat` once dragged the whole of
# goostd/strings AND goostd/utf8 through the -O2 pipeline and into the object:
# 30 functions, 2,855 IR lines. Measured with `opt`, marking them internal cut
# the -O2 pass time from 0.24 s to 0.02 s and left ONE function, because Repeat
# then inlines into main.
#
# Goo compiles a whole program into ONE module and has no separate compilation,
# so nothing outside the module can reference a `goo_pkg__*` symbol. Internal is
# therefore the correct linkage, and this probe is what stops it regressing to
# external — which would be invisible, because the program would still run
# correctly, only slower and larger.
#
# ASSERTS, so it belongs in verify-core. This is a property of the emitted IR,
# not a timing, so it is stable on any machine.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
COMPILER="${COMPILER:-$ROOT/bin/goo}"

fail() { echo "dead-package-code-probe: FAIL — $1"; exit 1; }

[ -x "$COMPILER" ] || fail "compiler not found at $COMPILER (run 'make')"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

# Calls exactly one function from strings. Everything else in strings, and all
# of utf8 (which strings imports), is unreachable.
cat > "$WORKDIR/one.goo" <<'EOF'
package main

import "fmt"
import "strings"

func main() {
	fmt.Println(strings.Repeat("ab", 3))
}
EOF

"$COMPILER" -O2 --emit-llvm -o "$WORKDIR/one.ll" "$WORKDIR/one.goo" >"$WORKDIR/err.txt" 2>&1 \
    || { sed 's/^/    /' "$WORKDIR/err.txt"; fail "compile failed"; }

echo "=== dead-package-code-probe: unused package functions must not survive -O2 ==="

# The program must still be correct. A probe that only counts functions would
# pass on a compiler that dropped code it should have kept.
"$COMPILER" -O2 -o "$WORKDIR/one" "$WORKDIR/one.goo" >/dev/null 2>&1 || fail "executable build failed"
got="$("$WORKDIR/one")"
[ "$got" = "ababab" ] || fail "wrong output: got '$got', expected 'ababab'"
echo "  output correct: ababab"

# These are never called. Each one surviving means globaldce could not see it.
DEAD="goo_pkg__strings__Count goo_pkg__strings__Index goo_pkg__strings__HasPrefix
      goo_pkg__strings__EqualFold goo_pkg__utf8__RuneLen goo_pkg__utf8__DecodeRune"

survivors=0
for sym in $DEAD; do
    if grep -q "^define[^@]*@${sym}(" "$WORKDIR/one.ll"; then
        echo "  SURVIVED (should have been removed): $sym"
        survivors=$((survivors + 1))
    fi
done

total_defs="$(grep -c '^define' "$WORKDIR/one.ll")"
echo "  total functions defined in the -O2 IR: $total_defs"

[ "$survivors" -eq 0 ] || fail "$survivors unreachable package function(s) survived -O2"

# A ceiling, not an exact count, so ordinary codegen changes do not churn this.
# The pre-fix number was 30. Anything near that means linkage regressed.
[ "$total_defs" -le 8 ] || fail "too many functions in the IR ($total_defs > 8) — package linkage has regressed to external"

echo "dead-package-code-probe: PASS (no unreachable package function survived, $total_defs definitions total)"
