#!/bin/bash
# lexer-keyword probe — the 25 words removed from token.c's keyword table on
# 2026-08-17 must stay ordinary identifiers, and no word may go back to being
# silently dropped.
#
# WHAT WENT WRONG. src/lexer/token.c mapped 25 words to TOKEN_* values that
# src/parser/lexer_bridge.c never mapped to a bison token. The bridge's unmapped
# arm is:
#
#     if (bison_token == -1) {
#         // Skip unknown tokens
#         return bridge_next_mapped();
#     }
#
# so the token VANISHED before the parser saw it. Two failures fell out:
#
#   1. SILENT ACCEPT. `gpu_kernel func f() {}` compiled as an ordinary function
#      and `wasm_export` alone on a line compiled with exit 0. This is the hole
#      P0.4 closed for TOKEN_UNKNOWN, reopened through the keyword table.
#      gpu-kernel-reject-probe did not catch it: its fixture omits `func`, so it
#      failed for an unrelated reason.
#   2. STOLEN IDENTIFIERS. `wasm_memory := 3` was a PARSE ERROR. A user could
#      not name a variable any of these 25 words, for features that do not
#      exist.
#
# TWO-SIDED, deliberately. Side A asserts the words are usable; side B asserts
# an undefined name still fails. Without side B, side A would keep passing if
# the compiler stopped rejecting anything at all.

set -u

PROBE="lexer-keyword-probe"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
COMPILER="${COMPILER:-$ROOT/bin/goo}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

WORDS="asm extern msg_from ll_volatile ll_inline no_std par_parallel par_reduce
par_barrier par_atomic par_thread_local gpu_kernel gpu_device gpu_host
gpu_global gpu_shared_mem gpu_constant gpu_local wasm wasm_export wasm_memory
wasm_table wasm_start wasm_elem wasm_data"

fail=0
n=0

# --- side A: every freed word works as an ordinary variable name ------------
for w in $WORDS; do
    n=$((n + 1))
    src="$TMP/id_$w.goo"
    printf 'package main\n\nimport "fmt"\n\nfunc main() {\n\t%s := 41\n\tfmt.Println(%s + 1)\n}\n' "$w" "$w" > "$src"
    if ! "$COMPILER" -o "$TMP/id_$w.bin" "$src" > "$TMP/id_$w.log" 2>&1; then
        echo "$PROBE: FAIL ('$w' is not usable as an identifier)"
        sed 's/^/  /' "$TMP/id_$w.log"
        fail=1
        continue
    fi
    got="$("$TMP/id_$w.bin" 2>/dev/null)"
    if [ "$got" != "42" ]; then
        echo "$PROBE: FAIL ('$w' compiled but printed '$got', want 42)"
        fail=1
    fi
done

# --- side B: an unknown name must still be REJECTED, loudly ------------------
# Without this, side A passes vacuously if the compiler accepts anything.
printf 'package main\n\nfunc main() {\n\tdefinitely_not_declared_xyz\n}\n' > "$TMP/undef.goo"
rm -f "$TMP/undef.bin"
"$COMPILER" -o "$TMP/undef.bin" "$TMP/undef.goo" > "$TMP/undef.out" 2> "$TMP/undef.err"
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "$PROBE: FAIL (control) an undeclared identifier compiled with exit 0."
    echo "  Side A above would pass vacuously, so this is fatal."
    fail=1
fi
if [ -e "$TMP/undef.bin" ]; then
    echo "$PROBE: FAIL (control) a binary was produced for an undeclared identifier"
    fail=1
fi

# --- side B2: the exact former-keyword regression ---------------------------
# `wasm_export` as a bare statement used to compile with exit 0.
printf 'package main\n\nfunc main() {\n\twasm_export\n}\n' > "$TMP/bare.goo"
rm -f "$TMP/bare.bin"
"$COMPILER" -o "$TMP/bare.bin" "$TMP/bare.goo" > "$TMP/bare.out" 2> "$TMP/bare.err"
if [ $? -eq 0 ]; then
    echo "$PROBE: FAIL ('wasm_export' as a bare statement compiled with exit 0 —"
    echo "  the silent-drop regression is back; check lexer_bridge.c's unmapped arm)"
    fail=1
fi

# --- side B3: gpu_kernel must not sneak a function past the parser ----------
printf 'package main\n\nfunc main() {}\n\ngpu_kernel func vadd(n int) {\n\tn = n + 1\n}\n' > "$TMP/gk.goo"
rm -f "$TMP/gk.bin"
"$COMPILER" -o "$TMP/gk.bin" "$TMP/gk.goo" > "$TMP/gk.out" 2> "$TMP/gk.err"
if [ $? -eq 0 ]; then
    echo "$PROBE: FAIL ('gpu_kernel func ...' compiled — a GPU kernel was silently"
    echo "  accepted as an ordinary CPU function)"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi

echo "$PROBE: PASS ($n freed identifiers usable, undefined names still rejected, no silent drop)"
exit 0
