#!/usr/bin/env bash
# Assert tests/probes/generated.bzl is current.
#
# The file holds 63 probe targets derived from the Makefile's own recipes, and
# its header says "do not edit". Nothing enforced that. A hand edit survived,
# and the derivation quietly became a snapshot -- which is the failure the
# whole generator exists to prevent, since its refusals are what found three
# wrong parse rules and a missing category.
#
# The generator reads only Makefile and census.txt, through awk. No bazel and
# no compiler, so unlike census_current.sh this gate needs no `manual` tag.
#
# Exit: 0 current, 1 stale, 2 the generator failed.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GEN="tools/gen_probe_targets.py"
CENSUS="tests/probes/census.txt"
TARGET="tests/probes/generated.bzl"

# --------------------------------------------------------------------------
# Three mutations plus a CONTROL. Every one runs against a COPY: the plan's
# acceptance for this task requires the work tree clean afterwards, and
# census_current.sh's shape (operate on $root directly) cannot give that.
# --------------------------------------------------------------------------
if [ "${1:-}" = "--self-test" ]; then
    SELF="targets_current --self-test"
    W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
    bad=0

    mk() {
        rm -rf "$W/t"; mkdir -p "$W/t/tests/probes" "$W/t/tools"
        cp "$ROOT/Makefile"      "$W/t/"
        cp "$ROOT/$CENSUS"       "$W/t/$CENSUS"
        cp "$ROOT/$TARGET"       "$W/t/$TARGET"
        cp "$ROOT/$GEN"          "$W/t/$GEN"
        cp "${BASH_SOURCE[0]}"   "$W/t/tests/probes/targets_current.sh"
    }
    run() { ( cd "$W/t" && bash tests/probes/targets_current.sh ) >"$W/out.log" 2>&1; }

    mk
    if run; then echo "    ok: control (unmutated copy) is GREEN"
    else echo "$SELF: FAIL (control already red -- the harness is broken, not the tree)"
         sed 's/^/        /' "$W/out.log"; exit 1; fi

    check() {
        mk; ( cd "$W/t" && eval "$2" ) >/dev/null 2>&1
        if run; then echo "$SELF: FAIL (stayed green after: $1)"
             sed 's/^/        /' "$W/out.log"; bad=1
        else echo "    ok: '$1' turns it red"; fi
    }

    check "one renamed target in generated.bzl" \
        "sed -i 's/baremod_reject_probe/baremod_reject_probe_EDITED/' $TARGET"
    check "one deleted line from generated.bzl" \
        "sed -i '\$d' $TARGET"
    check "a generator that exits non-zero" \
        "printf 'import sys\nsys.exit(3)\n' > $GEN"

    [ "$bad" = 0 ] || exit 1
    echo "PASS: $SELF (control green, 3 mutations red)"
    # The real file must be untouched. Task 7 asks for exactly this.
    exit 0
fi

cd "$ROOT" || exit 2

W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT

python3 "$GEN" "$CENSUS" > "$W/fresh.bzl" 2>/dev/null
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "targets_current: TOOL FAILURE $GEN exited $rc"
    exit 2
fi

if diff -u "$TARGET" "$W/fresh.bzl" > "$W/diff" 2>&1; then
    echo "targets_current: PASS $(grep -c '        name = ' "$TARGET") targets, generated.bzl is current"
    exit 0
fi

echo "targets_current: FAIL $TARGET is stale"
head -20 "$W/diff"
echo "  regenerate with: python3 $GEN $CENSUS > $TARGET"
exit 1
