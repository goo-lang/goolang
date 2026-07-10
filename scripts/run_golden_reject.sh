#!/usr/bin/env bash
# Data-driven compile-reject runner: every tests/golden/reject/<name>.goo is
# compiled via bin/goo and MUST be rejected. A fixture independently fails
# on:
#   - acceptance: the compiler exits 0 (a program that should have been
#     rejected was silently accepted).
#   - leftover binary: the output path exists after the compile attempt,
#     even though the compiler reported failure — a reject must leave no
#     binary behind.
#   - missing/empty .err.txt sidecar: every *.goo here MUST carry a sidecar
#     naming its expected diagnostic substring. An empty sidecar would
#     vacuously match any stderr via `grep -qF`, so it is rejected loudly
#     here rather than silently passing (same class of bug run_golden.sh's
#     review flagged for .stderr.txt).
#   - stderr-substring miss: the sidecar's contents (fixed string, matched
#     with `grep -qF --`) do not appear in the captured stderr. Sidecar
#     contract: single-line — grep -F treats each line of a multi-line
#     pattern as an independent alternative, silently weakening the check.
# Exit non-zero iff any case fails.
#
# Exit statuses are always captured directly off the invocation (`rc=$?`),
# never through a pipe — piped exit codes reflect the pipe's last stage, not
# the compiler under test, and would silently mask aborts here. Same
# convention as run_golden.sh.
set -u
COMPILER="${COMPILER:-bin/goo}"
REJECT_DIR="${REJECT_DIR:-tests/golden/reject}"
# P3.10: same GOOFLAGS passthrough as run_golden.sh, for symmetry; default
# empty = today's behavior, unchanged. This suite doesn't gate on it (reject
# fixtures don't depend on optimization level) but keeping it callable the
# same way avoids surprises if a caller scripts both runners identically.
GOOFLAGS="${GOOFLAGS:-}"
pass=0; fail=0; failed=()
for goo in "$REJECT_DIR"/*.goo; do
    [ -f "$goo" ] || continue
    base="$(basename "$goo" .goo)"
    errfile="${goo%.goo}.err.txt"
    out_bin="build/reject_${base}.out"
    out_stdout="build/reject_${base}.stdout.txt"
    out_stderr="build/reject_${base}.stderr.txt"
    mkdir -p build
    rm -f "$out_bin"

    if [ ! -f "$errfile" ]; then
        echo "FAIL  $base (missing .err.txt sidecar)"
        fail=$((fail+1)); failed+=("$base"); continue
    fi
    if ! grep -q '[^[:space:]]' "$errfile"; then
        # -s alone misses whitespace-only sidecars: command substitution
        # strips the trailing newline and `grep -qF -- ""` matches anything.
        echo "FAIL  $base (empty/whitespace-only .err.txt sidecar — would vacuously match any stderr)"
        fail=$((fail+1)); failed+=("$base"); continue
    fi

    # $GOOFLAGS intentionally unquoted; see run_golden.sh for rationale.
    "$COMPILER" "$goo" -o "$out_bin" $GOOFLAGS >"$out_stdout" 2>"$out_stderr"
    rc=$?

    if [ "$rc" -eq 0 ]; then
        echo "FAIL  $base (accepted rc=0 — expected rejection)"
        fail=$((fail+1)); failed+=("$base"); continue
    fi

    if [ -e "$out_bin" ]; then
        echo "FAIL  $base (binary emitted at $out_bin despite rejection)"
        fail=$((fail+1)); failed+=("$base"); continue
    fi

    # Global negative assertion (spec: "no LLVM verifier noise reaching
    # users"): a rejection whose diagnostic is followed by raw verifier
    # output means the checker's reject path still fell through to codegen.
    # Carried over from the deleted constdiv/funcsig probe recipes, applied
    # to every fixture uniformly.
    if grep -qiE 'Module verification failed|LLVM ERROR' "$out_stderr"; then
        echo "FAIL  $base (LLVM verifier noise in rejection stderr)"
        sed 's/^/    /' "$out_stderr" | head -10
        fail=$((fail+1)); failed+=("$base"); continue
    fi

    # Global negative assertion (P2.8 FIX F1): TYPE_POISON's internal
    # "<poisoned>" marker (see types.c) exists purely to let the checker
    # recover scope state after a failed declaration — it must never reach
    # a user-facing diagnostic. Its presence here means some diagnostic
    # site is missing a type_is_poison guard (cascade-suppression
    # regression). Applied to every fixture uniformly, mirroring the
    # LLVM-noise check above.
    if grep -qF -- '<poisoned>' "$out_stderr"; then
        echo "FAIL  $base (internal <poisoned> marker leaked into stderr)"
        sed 's/^/    /' "$out_stderr" | head -10
        fail=$((fail+1)); failed+=("$base"); continue
    fi

    if ! grep -qF -- "$(cat "$errfile")" "$out_stderr"; then
        echo "FAIL  $base (stderr mismatch: expected substring not found)"
        echo "  expected: $(cat "$errfile")"
        echo "  actual stderr:"
        sed 's/^/    /' "$out_stderr"
        fail=$((fail+1)); failed+=("$base"); continue
    fi

    echo "PASS  $base"; pass=$((pass+1))
done
echo "--- golden-reject: $pass passed, $fail failed ---"
# Floor: an empty fixture directory (e.g. a path typo silently matching
# nothing) must not report success — this suite exists to reject things.
if [ "$pass" -eq 0 ] && [ "$fail" -eq 0 ]; then
    echo "FAIL  golden-reject: no fixtures found under $REJECT_DIR"
    exit 1
fi
[ "$fail" -eq 0 ]
