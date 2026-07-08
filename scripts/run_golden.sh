#!/usr/bin/env bash
# Data-driven golden runner: every examples/<name>.goo with a sibling
# examples/<name>.expected.txt is compiled via bin/goo, run, and its stdout
# diffed against the expected file. A fixture also independently fails on:
#   - compile/link failure
#   - timeout: GOLDEN_TIMEOUT seconds (default 10), enforced via `timeout`
#     (default signal TERM). rc 124 is ALWAYS a timeout failure, never
#     compared against a `.exit` sidecar.
#   - exit-code mismatch: examples/<name>.exit, if present, holds a single
#     integer with the expected exit code; if absent, exit 0 is required.
#   - stderr-substring miss: examples/<name>.stderr.txt, if present, holds
#     a substring that must appear in the fixture's captured stderr
#     (checked with `grep -qF`).
# Exit non-zero iff any case fails.
#
# Env-dependent fixtures: if examples/<name>.env exists, its KEY=VAL lines are
# exported (only) for that fixture's run — so programs using os.Getenv (e.g.
# m12_probe) run end-to-end in the golden suite instead of being skipped.
#
# Exit statuses are always captured directly off the invocation (`rc=$?`),
# never through a pipe — piped exit codes reflect the pipe's last stage, not
# the program under test, and would silently mask aborts/timeouts here.
set -u
COMPILER="${COMPILER:-bin/goo}"
EX_DIR="${EX_DIR:-examples}"
GOLDEN_TIMEOUT="${GOLDEN_TIMEOUT:-10}"
pass=0; fail=0; failed=()
for goo in "$EX_DIR"/*.goo; do
    exp="${goo%.goo}.expected.txt"
    [ -f "$exp" ] || continue
    base="$(basename "$goo" .goo)"
    out_bin="build/golden_${base}.out"
    out_stdout="build/golden_${base}.stdout.txt"
    out_stderr="build/golden_${base}.stderr.txt"
    mkdir -p build
    if ! "$COMPILER" "$goo" -o "$out_bin" >/dev/null 2>"build/golden_${base}.cerr"; then
        echo "FAIL  $base (compile/link)"; fail=$((fail+1)); failed+=("$base"); continue
    fi

    # Run under `timeout`, capturing stdout/stderr to temp files and the
    # REAL exit code (rc=$? off the invocation, not a `$(...)` substitution
    # or a pipe) so a hang, an abort, and a clean exit are all distinguishable.
    envfile="${goo%.goo}.env"
    if [ -f "$envfile" ]; then
        # Source the fixture's env (KEY=VAL lines) in a subshell so it's
        # exported only for this run, not the runner itself.
        (set -a; . "$envfile"; set +a; exec timeout "$GOLDEN_TIMEOUT" "$out_bin") >"$out_stdout" 2>"$out_stderr"
    else
        timeout "$GOLDEN_TIMEOUT" "$out_bin" >"$out_stdout" 2>"$out_stderr"
    fi
    rc=$?
    actual="$(cat "$out_stdout")"

    if [ "$actual" != "$(cat "$exp")" ]; then
        echo "FAIL  $base (output mismatch)"
        diff <(printf '%s' "$(cat "$exp")") <(printf '%s' "$actual") | head -20
        fail=$((fail+1)); failed+=("$base"); continue
    fi

    if [ "$rc" -eq 124 ]; then
        echo "FAIL  $base (timeout)"
        fail=$((fail+1)); failed+=("$base"); continue
    fi

    exitfile="${goo%.goo}.exit"
    want_rc=0
    if [ -f "$exitfile" ]; then
        # Strip all whitespace (stray spaces/CRLF would make `[ -ne ]` error
        # out and silently skip this check) and demand pure digits.
        want_rc="$(tr -d '[:space:]' < "$exitfile")"
        case "$want_rc" in
            ''|*[!0-9]*)
                echo "FAIL  $base (malformed .exit sidecar: '$want_rc')"
                fail=$((fail+1)); failed+=("$base"); continue;;
        esac
    fi
    if [ "$rc" -ne "$want_rc" ]; then
        echo "FAIL  $base (exit code: got $rc, want $want_rc)"
        fail=$((fail+1)); failed+=("$base"); continue
    fi

    stderrfile="${goo%.goo}.stderr.txt"
    if [ -f "$stderrfile" ]; then
        # Same guard as run_golden_reject.sh: an empty/whitespace-only
        # sidecar would vacuously match any stderr via `grep -qF ""`.
        # Sidecar contract: single-line fixed string (grep -F treats each
        # line of a multi-line pattern as an independent alternative).
        if ! grep -q '[^[:space:]]' "$stderrfile"; then
            echo "FAIL  $base (empty/whitespace-only .stderr.txt sidecar)"
            fail=$((fail+1)); failed+=("$base"); continue
        fi
        if ! grep -qF -- "$(cat "$stderrfile")" "$out_stderr"; then
            echo "FAIL  $base (stderr mismatch)"
            fail=$((fail+1)); failed+=("$base"); continue
        fi
    fi

    echo "PASS  $base"; pass=$((pass+1))
done
echo "--- golden: $pass passed, $fail failed ---"
[ "$fail" -eq 0 ]
