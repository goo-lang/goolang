#!/bin/bash
# assert-corpus: run the whole fixture corpus through an ASSERT-ENABLED compiler.
#
# WHY THIS EXISTS. `make verify-core` builds the production compiler, where
# GOO_ASSERT compiles to (void)0. So every gate in verify-core can be green
# while every assert in the tree is dead code. An assert that no build ever
# evaluates is not a check, it is a comment with parentheses.
#
# This is the target that makes them earn their keep: build with GOO_DEBUG and
# push all ~751 fixtures through it, so each assert is evaluated on real input
# rather than on a synthetic probe.
#
# NOT IN verify-core, and the reason is mechanical rather than a judgement:
# `make debug` and `make lexer` both write bin/goo. Running this inside
# verify-core would swap the production binary out from under the other 197
# gates mid-run. It rebuilds bin/goo twice and takes several minutes, so it
# belongs in a pre-release sweep, not in the per-change gate.
#
# scripts/goo_assert_probe.sh is the fast companion and IS in verify-core: it
# checks that the three build modes behave, which is the property that would
# otherwise rot silently.
#
# LEAVES A DEBUG BINARY IN PLACE. Run `make lexer` afterwards, or let the next
# verify-core rebuild it. The script says so at the end rather than doing it
# silently, because a caller may want to keep the debug build to reproduce a
# failure.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

TIMEOUT="${FIXTURE_TIMEOUT:-30}"
MAX_REPORT="${MAX_REPORT:-10}"

# Overrides, used only by --self-test below. The default path is unchanged:
# build the real compiler with GOO_DEBUG and sweep the real corpus.
GOO_BIN="${ASSERT_CORPUS_BIN:-./bin/goo}"
SKIP_BUILD="${ASSERT_CORPUS_SKIP_BUILD:-0}"
FIXTURE_GLOB="${ASSERT_CORPUS_FIXTURES:-}"

# ---------------------------------------------------------------------------
# --self-test. This script had NO negative control until 2026-08-20, and it was
# invisible to probe-teeth-probe, which scanned scripts/*probe*.sh and cannot
# see a name without "probe" in it. So the rule "a new probe cannot enter this
# tree without teeth" did not reach the sweep that guards 754 fixtures.
#
# A STUB COMPILER, NOT A MUTATED SOURCE. release_decision_teeth.sh mutates a .c
# file and rebuilds, which works because that suite links one object. The unit
# under test HERE is the sweep -- "does this script notice a fixture that
# aborted" -- and the compiler is only what produces the abort. Rebuilding the
# real compiler three times to prove a return-code comparison costs about twelve
# minutes and tests the comparison no harder than a stub does.
#
# The build path is not left unguarded by that choice: the `asserts: on` check
# below is what proves GOO_DEBUG reached the objects, and its comment records
# two earlier versions of that check that were both wrong and both confident.
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--self-test" ]; then
    SELF="assert-corpus --self-test"
    W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
    bad_self=0

    mkdir -p "$W/fixtures" "$W/empty"
    for i in 1 2 3; do echo 'package main' > "$W/fixtures/f$i.goo"; done

    make_stub() {  # abort-on-suffix ("none" for clean), version line
        cat > "$W/goo" <<EOF
#!/usr/bin/env bash
if [ "\$1" = "--version" ]; then echo "Goo Compiler v0.0.0-stub"; echo "$2"; exit 0; fi
for a in "\$@"; do case "\$a" in *"$1") exit 134 ;; esac; done
exit 0
EOF
        chmod +x "$W/goo"
    }
    run_sweep() {  # fixture glob
        ASSERT_CORPUS_BIN="$W/goo" ASSERT_CORPUS_SKIP_BUILD=1 \
        ASSERT_CORPUS_FIXTURES="$1" "$0" > "$W/out.log" 2>&1
    }

    # CONTROL. Without it, a stub that cannot execute turns every row red and
    # reads as three successes.
    make_stub none "asserts: on"
    if run_sweep "$W/fixtures/*.goo"; then
        echo "    ok: control (clean stub, asserts on) is GREEN"
    else
        echo "$SELF: FAIL (control already red -- the harness is broken)"
        sed 's/^/        /' "$W/out.log"; exit 1
    fi

    make_stub "f2.goo" "asserts: on"
    if run_sweep "$W/fixtures/*.goo"; then
        echo "$SELF: FAIL (a fixture that aborted did not turn the sweep red)"
        sed 's/^/        /' "$W/out.log"; bad_self=1
    elif ! grep -q "f2.goo" "$W/out.log"; then
        echo "$SELF: FAIL (went red without naming the fixture that aborted)"
        sed 's/^/        /' "$W/out.log"; bad_self=1
    else
        echo "    ok: an aborting fixture turns it red, and is named"
    fi

    # The one that matters most: every assert compiled out means every fixture
    # passes and the verdict reads exactly like success.
    make_stub none "asserts: off"
    if run_sweep "$W/fixtures/*.goo"; then
        echo "$SELF: FAIL (a compiler with asserts OFF reported a clean sweep)"
        sed 's/^/        /' "$W/out.log"; bad_self=1
    elif ! grep -q "BROKEN" "$W/out.log"; then
        echo "$SELF: FAIL (asserts OFF went red, but not as BROKEN)"
        sed 's/^/        /' "$W/out.log"; bad_self=1
    else
        echo "    ok: a compiler with asserts off is BROKEN, not a clean sweep"
    fi

    make_stub none "asserts: on"
    if run_sweep "$W/empty/*.goo"; then
        echo "$SELF: FAIL (an empty corpus reported a clean sweep)"
        sed 's/^/        /' "$W/out.log"; bad_self=1
    else
        echo "    ok: an empty corpus is refused"
    fi

    [ "$bad_self" -ne 0 ] && exit 1
    echo "$SELF: PASS (control green; 3 failure modes independently turn it red)"
    exit 0
fi

echo "=== assert-corpus: the fixture corpus against a GOO_DEBUG compiler ==="

if [ "$SKIP_BUILD" != "1" ]; then
    echo "--- building with -DGOO_DEBUG (asserts ON, ~4x slower) ---"
    make clean >/dev/null 2>&1
    if ! make debug >"$WORK/build.log" 2>&1; then
        echo "assert-corpus: FAIL — the debug build did not compile"
        grep -E "error:" "$WORK/build.log" | head -10
        exit 1
    fi
fi

# Instrument check. If GOO_DEBUG did not actually reach the compiler's objects,
# every assert below is a no-op and this script would report a clean sweep
# while checking nothing.
#
# Ask the BINARY, do not guess from its bytes. Two earlier versions of this
# check were both wrong, and each would have reported confidently:
#
#   1. grep for "ASSERT FAILED". Never contiguous — goo_assert_fail prints
#      "goo: %s FAILED" with the kind as a separate argument. Reported BROKEN
#      against a debug build whose asserts demonstrably fired.
#   2. grep for the format string "goo: %s FAILED". Present in ALL THREE builds
#      (61 copies), because goo_assert_fail is an unreferenced static that gcc
#      keeps at -O0, and its literal sits in .rodata either way. Could not
#      distinguish anything.
#
# `goo --version` reports the build directly, so there is nothing to infer.
if ! "$GOO_BIN" --version 2>/dev/null | grep -q "^asserts: on"; then
    echo "assert-corpus: BROKEN — $GOO_BIN reports:"
    "$GOO_BIN" --version 2>&1 | sed 's/^/      /'
    echo "  GOO_DEBUG did not reach the build, so every assert is compiled out."
    echo "  A clean sweep here would mean nothing."
    exit 1
fi
echo "  $GOO_BIN reports: $("$GOO_BIN" --version | grep '^asserts:')"

if [ -n "$FIXTURE_GLOB" ]; then
    mapfile -t FIXTURES < <(ls $FIXTURE_GLOB 2>/dev/null)
else
    mapfile -t FIXTURES < <(ls examples/*.goo tests/golden/reject/*.goo 2>/dev/null)
fi
[ "${#FIXTURES[@]}" -gt 0 ] || { echo "assert-corpus: BROKEN — empty corpus"; exit 1; }

echo "--- compiling ${#FIXTURES[@]} fixtures ---"
aborted=0
ran=0
for f in "${FIXTURES[@]}"; do
    timeout "$TIMEOUT" "$GOO_BIN" -o "$WORK/out" "$f" >/dev/null 2>"$WORK/err"
    rc=$?
    ran=$((ran + 1))
    # 128+SIGABRT(6)=134. A reject fixture exits 1 by design, and that is not a
    # failure here: this script asks whether an INVARIANT broke, not whether the
    # program compiled.
    if [ "$rc" -ge 134 ] && [ "$rc" -le 139 ]; then
        aborted=$((aborted + 1))
        if [ "$aborted" -le "$MAX_REPORT" ]; then
            echo "  ABORT rc=$rc  $f"
            sed 's/^/      /' "$WORK/err" | head -3
        fi
    fi
done

echo
echo "  fixtures compiled: $ran"
echo "  assert aborts:     $aborted"
[ "$aborted" -gt "$MAX_REPORT" ] && echo "  (only the first $MAX_REPORT are shown)"
echo
echo "  NOTE: bin/goo is now a DEBUG build. Run 'make lexer' to restore it."

if [ "$aborted" -eq 0 ]; then
    echo "assert-corpus: PASS ($ran fixtures, no invariant broken)"
    exit 0
fi
echo "assert-corpus: FAIL ($aborted fixture(s) broke an invariant)"
exit 1
