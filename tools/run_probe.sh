#!/usr/bin/env bash
# Compile one .goo fixture, run it, and check the result.
#
# This MIRRORS scripts/run_golden.sh's run_one_fixture contract deliberately,
# so the Make side and the Bazel side cannot disagree about what a fixture
# means. Every guard below exists because that file already learned it:
#
#   - exit status is taken with `rc=$?` directly off the invocation, never
#     through a pipe or $(...), so a hang, an abort and a clean exit stay
#     distinguishable;
#   - rc 124 is ALWAYS a timeout failure, never compared against an expected
#     exit code;
#   - an expected exit code must be pure digits, or it is a malformed
#     assertion rather than a silently skipped check;
#   - a stderr substring must contain a non-space character, because
#     `grep -qF ""` matches anything.
#
# One guard is new here. run_golden.sh discovers its assertions from sidecar
# FILES, so a fixture always has at least an .expected.txt. Here they arrive as
# ARGUMENTS, so a target can be declared with none at all -- which would
# compile, run, and assert nothing while reporting PASS. That is refused.
#
# Exit: 0 pass, 1 the probe failed, 2 the harness was misused.
set -uo pipefail

COMPILER=""; ARCHIVE=""; GOOROOT_DIR=""; SRC=""
EXPECTED=""; WANT_RC=""; STDERR_HAS=""; STDOUT_HAS=""
GOOFLAGS_IN=""; TIMEOUT=10; REJECT=0

while [ $# -gt 0 ]; do
    case "$1" in
        --compiler)        COMPILER="$2"; shift 2 ;;
        --archive)         ARCHIVE="$2"; shift 2 ;;
        --gooroot)         GOOROOT_DIR="$2"; shift 2 ;;
        --src)             SRC="$2"; shift 2 ;;
        --expected)        EXPECTED="$2"; shift 2 ;;
        --exit)            WANT_RC="$2"; shift 2 ;;
        --stderr-contains) STDERR_HAS="$2"; shift 2 ;;
        --stdout-contains) STDOUT_HAS="$2"; shift 2 ;;
        --gooflags)        GOOFLAGS_IN="$2"; shift 2 ;;
        --timeout)         TIMEOUT="$2"; shift 2 ;;
        --reject)          REJECT=1; shift ;;
        *) echo "run_probe: unknown argument '$1'"; exit 2 ;;
    esac
done

for req in COMPILER ARCHIVE SRC; do
    if [ -z "${!req}" ]; then echo "run_probe: --${req,,} is required"; exit 2; fi
done
[ -x "$COMPILER" ] || { echo "run_probe: compiler '$COMPILER' is not executable"; exit 2; }
[ -r "$ARCHIVE" ]  || { echo "run_probe: archive '$ARCHIVE' is not readable"; exit 2; }
[ -r "$SRC" ]      || { echo "run_probe: source '$SRC' is not readable"; exit 2; }

# A reject probe must name the diagnostic it expects. "the compile failed" is
# satisfied by a compiler that crashes, a missing file, or a typo in the
# fixture name -- naming the message is what makes it an assertion about the
# language rather than about the build.
if [ "$REJECT" -eq 1 ] && [ -z "$STDERR_HAS" ]; then
    echo "run_probe: --reject requires --stderr-contains"
    exit 2
fi

# A probe that asserts nothing passes vacuously. Refuse it.
if [ "$REJECT" -eq 0 ] && [ -z "$EXPECTED" ] && [ -z "$WANT_RC" ] && [ -z "$STDERR_HAS" ] && [ -z "$STDOUT_HAS" ]; then
    echo "run_probe: no assertion given (need --expected, --exit, --stdout-contains or --stderr-contains)"
    exit 2
fi
if [ -n "$EXPECTED" ] && [ ! -r "$EXPECTED" ]; then
    echo "run_probe: expected file '$EXPECTED' is missing"
    exit 2
fi
if [ -n "$WANT_RC" ]; then
    case "$WANT_RC" in ''|*[!0-9]*) echo "run_probe: malformed --exit '$WANT_RC'"; exit 2 ;; esac
fi
for s in "$STDERR_HAS" "$STDOUT_HAS"; do
    if [ -n "$s" ] && ! printf '%s' "$s" | grep -q '[^[:space:]]'; then
        echo "run_probe: a whitespace-only substring matches anything"; exit 2
    fi
done

base="$(basename "$SRC" .goo)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

export GOO_RUNTIME="$ARCHIVE"
[ -n "$GOOROOT_DIR" ] && export GOOROOT="$GOOROOT_DIR"

# shellcheck disable=SC2086
"$COMPILER" "$SRC" -o "$work/$base" $GOOFLAGS_IN >"$work/cout" 2>"$work/cerr"
crc=$?

if [ "$REJECT" -eq 1 ]; then
    # Four assertions, and the fourth is the one that distinguishes a language
    # rejection from a compiler crash. Both give a non-zero exit.
    if [ "$crc" -eq 0 ]; then
        echo "run_probe: FAIL $base (compiled cleanly; it must be rejected)"
        exit 1
    fi
    if [ -e "$work/$base" ]; then
        echo "run_probe: FAIL $base (emitted a binary despite the error)"
        exit 1
    fi
    if grep -qiE "Module verification failed|LLVM ERROR" "$work/cerr"; then
        echo "run_probe: FAIL $base (invalid IR reached the LLVM verifier instead of a clean rejection)"
        head -20 "$work/cerr"
        exit 1
    fi
    if ! grep -qF -- "$STDERR_HAS" "$work/cerr"; then
        echo "run_probe: FAIL $base (wrong or missing diagnostic; wanted '"'"'$STDERR_HAS'"'"')"
        head -20 "$work/cerr"
        exit 1
    fi
    echo "run_probe: PASS $base (rejected cleanly)"
    exit 0
fi

if [ "$crc" -ne 0 ]; then
    echo "run_probe: FAIL $base (compile/link)"
    head -20 "$work/cerr"
    exit 1
fi

timeout "$TIMEOUT" "$work/$base" >"$work/stdout" 2>"$work/stderr"
rc=$?

# Always a timeout, never compared against --exit.
if [ "$rc" -eq 124 ]; then echo "run_probe: FAIL $base (timeout after ${TIMEOUT}s)"; exit 1; fi

if [ -n "$EXPECTED" ] && [ "$(cat "$work/stdout")" != "$(cat "$EXPECTED")" ]; then
    echo "run_probe: FAIL $base (output mismatch)"
    diff -u "$EXPECTED" "$work/stdout" | head -20
    exit 1
fi
want="${WANT_RC:-0}"
if [ "$rc" -ne "$want" ]; then
    echo "run_probe: FAIL $base (exit code: got $rc, want $want)"
    head -10 "$work/stderr"
    exit 1
fi
if [ -n "$STDOUT_HAS" ] && ! grep -qF -- "$STDOUT_HAS" "$work/stdout"; then
    echo "run_probe: FAIL $base (stdout does not contain '$STDOUT_HAS')"
    head -10 "$work/stdout"; exit 1
fi
if [ -n "$STDERR_HAS" ] && ! grep -qF -- "$STDERR_HAS" "$work/stderr"; then
    echo "run_probe: FAIL $base (stderr does not contain '$STDERR_HAS')"
    head -10 "$work/stderr"; exit 1
fi

echo "run_probe: PASS $base"
exit 0
