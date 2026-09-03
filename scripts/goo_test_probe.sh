#!/usr/bin/env bash
# `goo test` end to end: discovery, _testmain synthesis, output format, exit
# status.
#
# TWO packages, because they pin different things and neither alone is enough:
#
#   examples/testpkg     — every test passes. Prints a single `ok`, exit 0.
#                          Also proves a test can call a function declared in a
#                          sibling NON-test file, which only holds if the
#                          synthesized _testmain.goo joined the package rather
#                          than forming a compilation unit of its own.
#   examples/testfailpkg — a failure plus a t.Fatal. Pins the --- FAIL headers,
#                          the indented file:line log lines, the silence of a
#                          passing test, and exit 1.
#
# The failing package is the load-bearing one. A passing run prints only `ok`,
# which a synthesized main that called NOTHING would also print; a --- FAIL
# header can only appear if a discovered test was really invoked by name.
#
# Durations are not reproducible, so every "N.NNs" is normalized to X.XXs
# before the diff. Exit codes are captured directly off the invocation and are
# NEVER read through a pipe — a pipeline reports its LAST stage's status, which
# would silently mask a compiler abort here.
set -u
# ROOT resolves one directory too deep under a Bazel sh_test, the same
# $0-derived bug the arc_* and arena_rss scripts had. Harmless here: ROOT
# only feeds the COMPILER default below, and the macro always sets
# COMPILER, so the wrong value is never read. Every other path in this
# script is relative to $PWD. Do not add a new $ROOT/... path here without
# checking that this still holds.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# COMPILER is the contract a Bazel sh_test uses to point this probe at the
# compiler it built: bin/goo does not exist inside the sandbox. Unset, this
# behaves exactly as it did.
COMPILER="${COMPILER:-$ROOT/bin/goo}"
case "$COMPILER" in
  /*) ;;
  *) COMPILER="$PWD/$COMPILER" ;;
esac

# GOO_RUNTIME too: the Bazel harness passes it relative to the runfiles
# root, and the cd into the package below would break it the same way.
case "${GOO_RUNTIME:-}" in
  "" | /*) ;;
  *) GOO_RUNTIME="$PWD/$GOO_RUNTIME"; export GOO_RUNTIME ;;
esac

# A runfiles tree is not a place to write, and nothing reads these files
# after the run — the failure paths below already print the diff inline.
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT
failures=0

norm() { sed -E 's/[0-9]+\.[0-9]+s/X.XXs/g'; }

# check_pkg <package-dir> <want-exit>
check_pkg() {
  local dir="$1" want_rc="$2"
  local name raw actual expected rc
  name="$(basename "$dir")"
  raw="$WORKDIR/$name.raw.txt"
  actual="$WORKDIR/$name.actual.txt"
  expected="$dir/expected.norm.txt"

  if [ ! -f "$expected" ]; then
    echo "goo-test-probe: FAIL ($name has no expected.norm.txt)"
    failures=$((failures + 1))
    return
  fi

  # The redirection is applied by THIS shell, so $raw stays the absolute
  # WORKDIR path while the compiler itself runs inside the package directory.
  ( cd "$dir" && "$COMPILER" test . ) > "$raw" 2>&1
  rc=$?

  norm < "$raw" > "$actual"

  if [ "$rc" -ne "$want_rc" ]; then
    echo "goo-test-probe: FAIL ($name exit $rc, want $want_rc)"
    sed 's/^/    /' "$actual"
    failures=$((failures + 1))
    return
  fi

  if ! diff -u "$expected" "$actual"; then
    echo "goo-test-probe: FAIL ($name output mismatch)"
    failures=$((failures + 1))
    return
  fi

  echo "  ok: $name (exit $rc, output matches)"
}

check_pkg examples/testpkg     0
check_pkg examples/testfailpkg 1

if [ "$failures" -ne 0 ]; then
  exit 1
fi
echo "goo-test-probe: PASS (discovery, synthesis, output format, exit status)"
