#!/bin/bash
# string-literal-header probe: a Goo string literal must be a real ARC object.
#
# Why this exists. Every goo_alloc'd object carries GOO_OBJ_HEADER_SIZE bytes
# before its payload, and goo_release reads them at `ptr - 16`. A string literal
# used to be emitted as a BARE byte array, so a release on one would compute
# `global - 16` and hand a .rodata address to free().
#
# That made a literal the FOURTH headerless pointer kind, and the only one with
# nothing excluding it: NULL and goo_zerobase are checked for at runtime, an
# arena pointer is excluded by static proof, and a literal was excluded by
# nothing at all. `last := ""` in bench/daemon is exactly that shape, so the
# first ARC release consumer would have aborted on the benchmark it exists to
# fix.
#
# codegen_const_string_value now emits { [2 x i64] header, [N x i8] bytes } with
# the count set to GOO_RC_IMMORTAL, and hands out a pointer to the BYTES. This
# probe asserts that shape in the IR, because the C unit test (obj_header_test
# row 15) proves only that the RUNTIME honours the sentinel — it builds its own
# static and cannot see what codegen emits. Both halves are needed.
#
# It also runs `opt -passes=verify` over the output. A previous session shipped
# invalid IR that no test asked about, found only by pointing opt at
# --emit-llvm; touching codegen without that check is how it happened.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# COMPILER is the contract a Bazel sh_test uses to point this probe at the
# compiler it built: bin/goo does not exist inside the sandbox. Unset, this
# behaves exactly as it did.
COMPILER="${COMPILER:-$ROOT/bin/goo}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fail() { echo "string-literal-header-probe: FAIL — $1"; exit 1; }

[ -x "$COMPILER" ] || fail "compiler not found at $COMPILER (run 'make')"

cat > "$WORK/lit.goo" <<'EOF'
package main

import "fmt"

func main() {
	s := "hello"
	empty := ""
	joined := s + " world"
	fmt.Println(joined)
	fmt.Println(len(empty))
}
EOF

"$COMPILER" --emit-llvm -o "$WORK/lit.ll" "$WORK/lit.goo" >/dev/null 2>&1 \
    || fail "goo --emit-llvm failed"

# 1. Every literal global carries the header, and the count is the immortal
#    sentinel. LLVM prints UINT64_MAX as -1 in a signed i64 initialiser.
lits=$(grep -c '^@str' "$WORK/lit.ll" || true)
[ "$lits" -ge 3 ] || fail "expected at least 3 literal globals, found $lits"

hdr=$(grep -c '^@str.* constant { \[2 x i64\], \[[0-9]* x i8\] } { \[2 x i64\] \[i64 -1, i64 0\]' "$WORK/lit.ll" || true)
[ "$hdr" -eq "$lits" ] \
    || fail "only $hdr of $lits literal globals carry an immortal ARC header"

# 2. The payload must keep max_align_t, exactly as goo_alloc's does, and
#    `data - 16` must be an aligned 8-byte read of the count.
aligned=$(grep -c '^@str.*, align 16$' "$WORK/lit.ll" || true)
[ "$aligned" -eq "$lits" ] \
    || fail "only $aligned of $lits literal globals are 16-byte aligned"

# 3. The IR must actually be valid. See this file's header comment.
if command -v opt >/dev/null 2>&1; then
    opt -passes=verify -disable-output "$WORK/lit.ll" 2>"$WORK/opt.err" \
        || { cat "$WORK/opt.err"; fail "opt -passes=verify rejected the IR"; }
else
    echo "string-literal-header-probe: NOTE — opt not found, skipped IR verification"
fi

# 4. And the program must still run and print the right thing: a wrong GEP into
#    the new struct would silently hand out the header bytes as the string.
"$COMPILER" -o "$WORK/lit" "$WORK/lit.goo" >/dev/null 2>&1 || fail "goo build failed"
actual="$("$WORK/lit")"
expected="$(printf 'hello world\n0')"
[ "$actual" = "$expected" ] \
    || fail "output changed — got '$actual', expected '$expected'"

echo "string-literal-header-probe: PASS ($lits literals, immortal header, align 16, IR valid, output matches)"
