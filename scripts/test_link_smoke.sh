#!/bin/bash
# Link smoke test (roadmap P0-7): prove the compiler takes a trivial program
# all the way to a *runnable* native executable.
#
# This is the end-to-end gate for Phase 0: source -> compile -> link -> run.
# It runs the compiler from an unrelated working directory to also guard
# cwd-independent runtime location (P0-2).

set -u

fail() {
    echo "FAIL: $1"
    exit 1
}

# Absolute compiler path so we can invoke it from an unrelated working
# directory: a real `goo` must locate its runtime regardless of cwd (P0-2).
COMPILER="$(cd "$(dirname "$0")/.." && pwd)/bin/goo"
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

SRC="$WORKDIR/smoke.goo"
EXE="$WORKDIR/smoke"

# Run everything from the temp dir, NOT the repo root, to catch cwd-relative
# object paths in the link step.
cd "$WORKDIR" || fail "could not enter work dir"

# Minimal valid program: an empty main. No statements, so this isolates the
# link step from any parser/codegen feature work.
printf 'package main\n\nfunc main() {\n}\n' > "$SRC"

if [ ! -x "$COMPILER" ]; then
    fail "compiler not found at $COMPILER (run 'make' first)"
fi

# Compile (and link) the program. We only care about the produced executable.
if ! "$COMPILER" "$SRC" > "$WORKDIR/compile.log" 2>&1; then
    sed 's/^/    /' "$WORKDIR/compile.log"
    fail "compiler returned non-zero for an empty main"
fi

if [ ! -x "$EXE" ]; then
    sed 's/^/    /' "$WORKDIR/compile.log"
    fail "no runnable executable was produced at $EXE"
fi

# The link must be clean: no executable-stack warning from the runtime (P0-6).
if grep -qi "executable stack" "$WORKDIR/compile.log"; then
    grep -i "executable stack" "$WORKDIR/compile.log" | sed 's/^/    /'
    fail "linker emitted an executable-stack warning"
fi

# Run it. An empty main must exit 0 and, by default, print NOTHING: a program's
# stdout must be its own output only, so golden stdout diffing is possible (P0-5).
"$EXE" > "$WORKDIR/run.out" 2>&1
status=$?
if [ "$status" -ne 0 ]; then
    sed 's/^/    /' "$WORKDIR/run.out"
    fail "executable exited with status $status (expected 0)"
fi
if [ -s "$WORKDIR/run.out" ]; then
    sed 's/^/    /' "$WORKDIR/run.out"
    fail "empty main printed to stdout by default (expected silent runtime)"
fi

# Under GOO_DEBUG the runtime banners must come back (opt-in diagnostics).
GOO_DEBUG=1 "$EXE" > "$WORKDIR/run.dbg" 2>&1
if [ ! -s "$WORKDIR/run.dbg" ]; then
    fail "GOO_DEBUG=1 produced no runtime diagnostics (expected banners)"
fi

echo "PASS: empty main compiled, linked, ran (exit 0), silent by default, verbose under GOO_DEBUG"
exit 0
