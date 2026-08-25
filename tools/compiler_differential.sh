#!/usr/bin/env bash
# Compare the Bazel-built compiler against the Make-built one on emitted IR.
#
# WHAT THIS IS. A migration-time check, not a permanent gate. It compares two
# BUILD SYSTEMS, so it cannot be hermetic, and the question it answers stops
# existing when the Makefile is deleted in phase 7. PHASE 7 DELETES THIS FILE.
#
# WHY IR RATHER THAN RUN BEHAVIOUR. --emit-llvm needs no runtime archive:
# verified 2026-08-25 that it exits 0 with GOO_RUNTIME=/nonexistent while a
# full compile fails at ld. That keeps NNG out of this phase entirely. It is
# also a STRONGER equivalence claim than matching stdout, because two
# different IRs can print the same thing.
#
# WHY NO NORMALISATION. The IR embeds ModuleID and source_filename, both
# derived from the -o BASENAME, plus a host-derived target triple. Give both
# compilers the same basename in different directories and the output is
# byte-identical. Verified 2026-08-25.
#
# WHY GOOROOT AND GOO_RUNTIME ARE BOTH PINNED. The compiler resolves TWO things
# relative to its own executable: the runtime archive (codegen.c) and the
# stdlib root (import_resolver.c). bin/goo sits beside goostd/ and finds it at
# <exe-dir>/../goostd; bazel-bin/src/compiler/goo does not, and falls back to
# "./goostd". That path is embedded verbatim in bounds-check diagnostic strings
# (@bc_file), so 51 of 495 fixtures -- every one importing a stdlib package --
# differed on the path alone with no codegen divergence at all. Pinning GOOROOT
# for BOTH compilers makes them embed the same string. This controls the
# variable rather than normalising it away, which keeps a real divergence in
# that same string visible.
#
# Exit codes: 0 every fixture identical, 1 a fixture differs, 2 a tool failed.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

MAKE_GOO="${MAKE_GOO:-bin/goo}"
BAZEL_GOO="${BAZEL_GOO:-bazel-bin/src/compiler/goo}"
EX_DIR="${EX_DIR:-examples}"

for c in "$MAKE_GOO" "$BAZEL_GOO"; do
    if [ ! -x "$c" ]; then
        echo "compiler_differential: TOOL FAILURE $c is not executable"
        echo "  build both first: make bin/goo && bazel build //src/compiler:goo"
        exit 2
    fi
done

# The goostd parent. Pinned identically for both compilers -- see the GOOROOT
# note above. GOOROOT's contract is the directory CONTAINING goostd/, which is
# the repo root in a dev checkout.
GOOROOT_PIN="${GOOROOT_PIN:-$root}"
if [ ! -d "$GOOROOT_PIN/goostd" ]; then
    echo "compiler_differential: TOOL FAILURE $GOOROOT_PIN/goostd does not exist"
    exit 2
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/a" "$work/b"

total=0; same=0; differ=0; failed=0
declare -a bad=()

for src in "$EX_DIR"/*.goo; do
    [ -f "${src%.goo}.expected.txt" ] || continue
    base="$(basename "$src" .goo)"
    total=$((total + 1))

    # Same BASENAME in both directories: that is what makes the IR comparable
    # with no normalisation.
    GOOROOT="$GOOROOT_PIN" GOO_RUNTIME=/nonexistent \
        "$MAKE_GOO"  --emit-llvm "$src" -o "$work/a/out.ll" >/dev/null 2>&1
    rc_a=$?
    GOOROOT="$GOOROOT_PIN" GOO_RUNTIME=/nonexistent \
        "$BAZEL_GOO" --emit-llvm "$src" -o "$work/b/out.ll" >/dev/null 2>&1
    rc_b=$?

    if [ "$rc_a" -ne "$rc_b" ]; then
        failed=$((failed + 1)); bad+=("$base (exit $rc_a vs $rc_b)"); continue
    fi
    if [ "$rc_a" -ne 0 ]; then
        # Both refused the same fixture. That is agreement, not a failure.
        same=$((same + 1)); continue
    fi
    if cmp -s "$work/a/out.ll" "$work/b/out.ll"; then
        same=$((same + 1))
    else
        differ=$((differ + 1)); bad+=("$base (IR differs)")
    fi
done

echo "compiler_differential: $total fixtures"
echo "  identical: $same"
echo "  differing: $differ"
echo "  exit-code mismatch: $failed"

# Empty-corpus guard. scripts/run_golden.sh had exactly this hole -- it exited
# 0 having compared nothing -- and 5c633f6 fixed it. Do not reintroduce it.
if [ "$total" -eq 0 ]; then
    echo "compiler_differential: TOOL FAILURE no fixtures found under $EX_DIR"
    exit 2
fi
if [ "$differ" -eq 0 ] && [ "$failed" -eq 0 ]; then
    echo "compiler_differential: PASS both compilers emit identical IR"
    exit 0
fi
printf '  %s\n' "${bad[@]}" | head -40
echo "compiler_differential: FAIL"
exit 1
