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
#
# P5.8: fixtures run in PARALLEL (GOLDEN_JOBS jobs, default nproc). This is
# safe because every fixture's on-disk state is keyed by its unique basename
# (build/golden_<base>.*) and fixtures never read each other's artifacts.
# Each fixture writes its verdict to build/golden_results/<base>.result; a
# serial aggregation pass then prints results in the same sorted-glob order
# as the old serial runner and computes the counts — pass/fail semantics,
# per-fixture messages, and the summary line are unchanged.
set -u

# ---------------------------------------------------------------------------
# --self-test: prove this runner can report the OPPOSITE result.
#
# It guards 495 fixtures and, until 2026-08-20, had never been shown able to
# fail. It was also invisible to probe-teeth-probe, which scanned
# scripts/*probe*.sh and cannot see a name without "probe" in it.
#
# REAL FIXTURES AND THE REAL COMPILER, not a stub. The unit under test is the
# whole per-fixture verdict chain -- compile, run, diff, timeout, .exit
# sidecar, .stderr sidecar -- and a stub compiler would only test the parts
# that do not involve compiling. Six tiny programs cost about two seconds.
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--self-test" ]; then
    SELF="run-golden --self-test"
    CC_GOO="${COMPILER:-bin/goo}"
    [ -x "$CC_GOO" ] || { echo "$SELF: BROKEN — $CC_GOO is not built"; exit 1; }
    W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
    bad_self=0
    checks_run=0

    prog() { printf 'package main\n\nimport "fmt"\n\nfunc main() {\n    fmt.Println("ok")\n}\n' > "$1"; }

    # One fixture dir per row, so a row can never inherit another's sidecars.
    new_dir() { d="$W/$1"; rm -rf "$d"; mkdir -p "$d"; }

    run_golden_on() {  # dir -> writes $W/out.log, echoes the exit status
        GOLDEN_RESULTS_DIR="$W/results" EX_DIR="$1" COMPILER="$CC_GOO" \
            "$0" > "$W/out.log" 2>&1
        echo $?
    }

    row() {  # name, dir, want_status, want_grep
        checks_run=$((checks_run + 1))
        local got; got="$(run_golden_on "$2")"
        if [ "$got" != "$3" ]; then
            echo "$SELF: FAIL ($1: exit $got, expected $3)"
            sed 's/^/        /' "$W/out.log"; bad_self=1; return
        fi
        if [ -n "$4" ] && ! grep -qF -- "$4" "$W/out.log"; then
            echo "$SELF: FAIL ($1: output does not contain '$4')"
            sed 's/^/        /' "$W/out.log"; bad_self=1; return
        fi
        echo "    ok: $1"
    }

    # CONTROL. Without it, a temp tree the compiler cannot read turns every row
    # red and reads as five successes.
    new_dir control; prog "$d/gt_ok.goo"; printf 'ok\n' > "$d/gt_ok.expected.txt"
    row "control: a matching fixture passes" "$d" 0 "1 passed, 0 failed"

    new_dir mismatch; prog "$d/gt_mismatch.goo"; printf 'NOT ok\n' > "$d/gt_mismatch.expected.txt"
    row "a wrong .expected.txt is caught, and named" "$d" 1 "FAIL  gt_mismatch (output mismatch)"

    new_dir exitcode; prog "$d/gt_exit.goo"; printf 'ok\n' > "$d/gt_exit.expected.txt"
    printf '7\n' > "$d/gt_exit.exit"
    row "an .exit sidecar mismatch is caught" "$d" 1 "gt_exit (exit code: got 0, want 7)"

    new_dir malformed; prog "$d/gt_bad_exit.goo"; printf 'ok\n' > "$d/gt_bad_exit.expected.txt"
    printf 'seven\n' > "$d/gt_bad_exit.exit"
    row "a malformed .exit sidecar is caught" "$d" 1 "malformed .exit sidecar"

    new_dir stderrmiss; prog "$d/gt_stderr.goo"; printf 'ok\n' > "$d/gt_stderr.expected.txt"
    printf 'this string never appears\n' > "$d/gt_stderr.stderr.txt"
    row "a missing .stderr substring is caught" "$d" 1 "gt_stderr (stderr mismatch)"

    # THE ROW THIS WAS WRITTEN FOR. A directory with no fixture at all must not
    # be a clean pass. Before 2026-08-20 it printed "0 passed, 0 failed" and
    # exited 0 -- one number away from "495 passed, 0 failed", in a line
    # nobody diffs.
    new_dir empty
    row "an empty corpus is refused, not reported as a clean sweep" "$d" 1 "BROKEN"

    [ "$bad_self" -ne 0 ] && exit 1
    echo "$SELF: PASS (control green; $((checks_run - 1)) failure modes independently turn it red)"
    exit 0
fi

COMPILER="${COMPILER:-bin/goo}"
EX_DIR="${EX_DIR:-examples}"
GOLDEN_TIMEOUT="${GOLDEN_TIMEOUT:-10}"
GOLDEN_JOBS="${GOLDEN_JOBS:-$(nproc 2>/dev/null || echo 4)}"
# P3.10: optional passthrough of extra compiler flags (e.g. "-O2") for the
# differential optimizer gate; default empty = today's behavior, unchanged.
GOOFLAGS="${GOOFLAGS:-}"

RESULTS_DIR="${GOLDEN_RESULTS_DIR:-build/golden_results}"
rm -rf "$RESULTS_DIR"
mkdir -p build "$RESULTS_DIR"

# Run ONE fixture; write its complete verdict (the exact lines the serial
# runner would have echoed) to $RESULTS_DIR/<base>.result. First line always
# starts with "PASS  " or "FAIL  " — the aggregator keys off that.
run_one_fixture() {
    local goo="$1"
    local exp="${goo%.goo}.expected.txt"
    local base res out_bin out_stdout out_stderr rc
    base="$(basename "$goo" .goo)"
    res="$RESULTS_DIR/${base}.result"
    out_bin="build/golden_${base}.out"
    out_stdout="build/golden_${base}.stdout.txt"
    out_stderr="build/golden_${base}.stderr.txt"

    # $GOOFLAGS intentionally unquoted: lets callers pass multiple flags
    # (e.g. "-O2 -v") that need independent word-splitting; empty by
    # default so this is a no-op unless a caller sets it.
    if ! "$COMPILER" "$goo" -o "$out_bin" $GOOFLAGS >/dev/null 2>"build/golden_${base}.cerr"; then
        echo "FAIL  $base (compile/link)" > "$res"; return
    fi

    # Run under `timeout`, capturing stdout/stderr to temp files and the
    # REAL exit code (rc=$? off the invocation, not a `$(...)` substitution
    # or a pipe) so a hang, an abort, and a clean exit are all distinguishable.
    local envfile="${goo%.goo}.env"
    if [ -f "$envfile" ]; then
        # Source the fixture's env (KEY=VAL lines) in a subshell so it's
        # exported only for this run, not the runner itself.
        (set -a; . "$envfile"; set +a; exec timeout "$GOLDEN_TIMEOUT" "$out_bin") >"$out_stdout" 2>"$out_stderr"
    else
        timeout "$GOLDEN_TIMEOUT" "$out_bin" >"$out_stdout" 2>"$out_stderr"
    fi
    rc=$?
    local actual
    actual="$(cat "$out_stdout")"

    if [ "$actual" != "$(cat "$exp")" ]; then
        {
            echo "FAIL  $base (output mismatch)"
            diff <(printf '%s' "$(cat "$exp")") <(printf '%s' "$actual") | head -20
        } > "$res"
        return
    fi

    if [ "$rc" -eq 124 ]; then
        echo "FAIL  $base (timeout)" > "$res"; return
    fi

    local exitfile="${goo%.goo}.exit"
    local want_rc=0
    if [ -f "$exitfile" ]; then
        # Strip all whitespace (stray spaces/CRLF would make `[ -ne ]` error
        # out and silently skip this check) and demand pure digits.
        want_rc="$(tr -d '[:space:]' < "$exitfile")"
        case "$want_rc" in
            ''|*[!0-9]*)
                echo "FAIL  $base (malformed .exit sidecar: '$want_rc')" > "$res"; return;;
        esac
    fi
    if [ "$rc" -ne "$want_rc" ]; then
        echo "FAIL  $base (exit code: got $rc, want $want_rc)" > "$res"; return
    fi

    local stderrfile="${goo%.goo}.stderr.txt"
    if [ -f "$stderrfile" ]; then
        # Same guard as run_golden_reject.sh: an empty/whitespace-only
        # sidecar would vacuously match any stderr via `grep -qF ""`.
        # Sidecar contract: single-line fixed string (grep -F treats each
        # line of a multi-line pattern as an independent alternative).
        if ! grep -q '[^[:space:]]' "$stderrfile"; then
            echo "FAIL  $base (empty/whitespace-only .stderr.txt sidecar)" > "$res"; return
        fi
        if ! grep -qF -- "$(cat "$stderrfile")" "$out_stderr"; then
            echo "FAIL  $base (stderr mismatch)" > "$res"; return
        fi
    fi

    echo "PASS  $base" > "$res"
}
export -f run_one_fixture
export COMPILER EX_DIR GOLDEN_TIMEOUT GOOFLAGS RESULTS_DIR

# Collect the fixture list once (sorted glob order — same as the old serial
# loop), then fan out. NUL-delimited for safety; each xargs slot runs one
# fixture in a fresh bash so `set -u`/locals can't leak across fixtures.
fixtures=()
for goo in "$EX_DIR"/*.goo; do
    [ -f "${goo%.goo}.expected.txt" ] || continue
    fixtures+=("$goo")
done

# Instrument check. A corpus of nothing fails nothing, so the aggregation
# below would print "0 passed, 0 failed" and the final `[ "$fail" -eq 0 ]`
# would exit 0. Against the real "495 passed, 0 failed" that is one number in
# a line nobody diffs.
#
# The path that gets here is not a mistyped EX_DIR. It is the filter above: a
# .goo with no .expected.txt sibling is dropped SILENTLY and by design, because
# not every example is a golden fixture. So a renamed sidecar convention drops
# all 495 one at a time and leaves a clean sweep behind. coverage_corpus.sh has
# guarded exactly this since it was written; the runner that matters more never
# did.
if [ "${#fixtures[@]}" -eq 0 ]; then
    echo "run-golden: BROKEN — no fixture found in '$EX_DIR'."
    echo "  Nothing was compared. A '0 passed, 0 failed' verdict here would mean"
    echo "  the suite did not run, not that every fixture matched."
    echo "  A .goo is only a fixture if a sibling .expected.txt exists."
    exit 1
fi

printf '%s\0' "${fixtures[@]}" | xargs -0 -P "$GOLDEN_JOBS" -n 1 \
    bash -c 'run_one_fixture "$1"' _

# Serial aggregation in the original order: identical output stream and
# counts to the pre-P5.8 runner. A missing result file means the fixture's
# worker died without a verdict (e.g. OOM-kill) — that is a FAIL, never a
# silent skip.
pass=0; fail=0
for goo in "${fixtures[@]}"; do
    base="$(basename "$goo" .goo)"
    res="$RESULTS_DIR/${base}.result"
    if [ ! -f "$res" ]; then
        echo "FAIL  $base (no verdict — worker died)"
        fail=$((fail+1)); continue
    fi
    cat "$res"
    if head -1 "$res" | grep -q '^PASS  '; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
    fi
done
echo "--- golden: $pass passed, $fail failed ---"
[ "$fail" -eq 0 ]
