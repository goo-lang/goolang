#!/usr/bin/env bash
# proof-cache-shell probe: proof_cache_create() must not hand its argument to a
# shell.
#
# WHY THIS EXISTS. proof_cache_create() built a command with
#
#     snprintf(command, sizeof(command), "mkdir -p %s", cache_dir);
#     system(command);
#
# so a cache_dir containing `;` or a backtick ran arbitrary commands, and a path
# longer than 511 bytes was silently truncated. Found by the MISRA C:2012 scan
# on 2026-08-14 (Rule 21.8, deviation record docs/misra/deviations/D-09.md).
#
# The only caller passes a hard-coded "/tmp/goo_proof_cache", so nothing reached
# it. But `cache_directory` is a public char* in ProofGenerationContext
# (include/proof_generation.h), so reachability is one assignment away, and the
# repository already had the correct pattern in two other places
# (src/package/ai_cache.c:215 and the static helper in hybrid_registry.c).
#
# TWO-SIDED, deliberately. Asserting only "no command ran" passes vacuously if
# proof_cache_create() stops doing anything at all. So the probe also asserts
# the benign case still creates the directory. A probe whose control can read
# zero is not a probe.
set -u

PROBE="proof-cache-shell-probe"
# A Bazel sh_test starts in the runfiles root, and $0 there is a symlink
# under tests/probes/ -- one level too deep for the old dirname-based ROOT.
# git rev-parse names the real toplevel under make and by hand; it fails in
# the sandbox (no .git), where pwd is already the runfiles root.
ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# CC_PROBE first: the Bazel macro passes it from the toolchain it selected.
# CC is what the make recipe leaves in the environment ($(CC), Makefile:4633).
# A bare gcc is the last resort, for a person running this by hand.
CC_BIN="${CC_PROBE:-${CC:-gcc}}"

cat > "$TMP/harness.c" <<'EOF'
#include "proof_generation.h"
int main(int argc, char** argv)
{
    if (argc != 2) { return 2; }
    (void)proof_cache_create(argv[1]);
    return 0;
}
EOF

# The dependency set is the one runtime_optimization_demo already links.
if ! "$CC_BIN" -w -std=c23 -I"$ROOT" -I"$ROOT/include" -D_GNU_SOURCE \
    -include "$ROOT/include/xalloc.h" -include "$ROOT/include/goo_assert.h" \
    "$TMP/harness.c" \
    "$ROOT/src/types/proof_obligations.c" \
    "$ROOT/src/types/proof_smt.c" \
    "$ROOT/src/types/proof_reporting.c" \
    "$ROOT/src/types/contracts.c" \
    "$ROOT/src/types/dependent_types.c" \
    "$ROOT/src/types/symbolic_expression.c" \
    -o "$TMP/harness" -lm > "$TMP/build.log" 2>&1; then
    echo "$PROBE: FAIL (harness did not build)"
    sed 's/^/  /' "$TMP/build.log"
    exit 1
fi

fail=0

# --- control: the benign path must still create the directory ---------------
benign="$TMP/cache_ok"
"$TMP/harness" "$benign" > /dev/null 2>&1
if [ ! -d "$benign" ]; then
    echo "$PROBE: FAIL (control) proof_cache_create did not create '$benign'."
    echo "  The injection assertion below would pass vacuously, so this is fatal."
    fail=1
fi

# --- the actual gate: a shell metacharacter must not execute ----------------
sentinel="$TMP/pwned"
"$TMP/harness" "$TMP/cache_evil; touch $sentinel" > /dev/null 2>&1
if [ -e "$sentinel" ]; then
    echo "$PROBE: FAIL a ';' in cache_dir executed a command."
    echo "  proof_cache_create() reached a shell. Use mkdir(2), never system()."
    fail=1
fi

# A backtick is the other classic form and takes a different parse path.
sentinel2="$TMP/pwned2"
"$TMP/harness" "$TMP/cache_evil2\$(touch $sentinel2)" > /dev/null 2>&1
if [ -e "$sentinel2" ]; then
    echo "$PROBE: FAIL a '\$(...)' in cache_dir executed a command."
    fail=1
fi

if [ "$fail" -ne 0 ]; then exit 1; fi
echo "$PROBE: PASS (directory created, no shell reached)"
