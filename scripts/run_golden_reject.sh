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
#     with `grep -qF --`) do not appear in the captured stderr.
# Exit non-zero iff any case fails.
#
# Exit statuses are always captured directly off the invocation (`rc=$?`),
# never through a pipe — piped exit codes reflect the pipe's last stage, not
# the compiler under test, and would silently mask aborts here. Same
# convention as run_golden.sh.
set -u
COMPILER="${COMPILER:-bin/goo}"
REJECT_DIR="${REJECT_DIR:-tests/golden/reject}"
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
    if [ ! -s "$errfile" ]; then
        echo "FAIL  $base (empty .err.txt sidecar — would vacuously match any stderr)"
        fail=$((fail+1)); failed+=("$base"); continue
    fi

    "$COMPILER" "$goo" -o "$out_bin" >"$out_stdout" 2>"$out_stderr"
    rc=$?

    if [ "$rc" -eq 0 ]; then
        echo "FAIL  $base (accepted rc=0 — expected rejection)"
        fail=$((fail+1)); failed+=("$base"); continue
    fi

    if [ -e "$out_bin" ]; then
        echo "FAIL  $base (binary emitted at $out_bin despite rejection)"
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
[ "$fail" -eq 0 ]
