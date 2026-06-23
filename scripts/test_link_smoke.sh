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

# Run it. An empty main must exit 0.
"$EXE"
status=$?
if [ "$status" -ne 0 ]; then
    fail "executable exited with status $status (expected 0)"
fi

echo "PASS: empty main compiled, linked, and ran (exit 0)"
exit 0
