#!/usr/bin/env bash
# Runs the grammar conflict tripwire under Bazel.
#
# scripts/grammar-tripwire.sh already does the real work and already reads
# bison's exit status directly rather than through a pipeline. This wrapper
# exists only to locate the script and the grammar inside runfiles and to pass
# the grammar path as $1, which the script accepts.
#
# WHY THIS TARGET EXISTS AT ALL. CLAUDE.md states the tripwire as a hard gate:
# it "must PASS before AND after the change; any delta is stop-the-line". As of
# 2026-08-25 nothing ran it. There is no Makefile target (0 references), it is
# absent from VERIFY_ALL_DEPS, and no workflow invokes it -- the only mentions
# anywhere are three other scripts citing it as the model for a pinned
# baseline. It was the one baseline in the tree with no enforcement.
#
# Exit codes pass straight through: 0 baseline exact, 1 delta (STOP), 2 bison
# itself failed.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

exec bash scripts/grammar-tripwire.sh src/parser/parser.y
