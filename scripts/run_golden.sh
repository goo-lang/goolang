#!/usr/bin/env bash
# Data-driven golden runner: every examples/<name>.goo with a sibling
# examples/<name>.expected.txt is compiled via bin/goo, run, and its stdout
# diffed against the expected file. Exit non-zero iff any case fails.
#
# Env-dependent fixtures: if examples/<name>.env exists, its KEY=VAL lines are
# exported (only) for that fixture's run — so programs using os.Getenv (e.g.
# m12_probe) run end-to-end in the golden suite instead of being skipped.
set -u
COMPILER="${COMPILER:-bin/goo}"
EX_DIR="${EX_DIR:-examples}"
pass=0; fail=0; failed=()
for goo in "$EX_DIR"/*.goo; do
    exp="${goo%.goo}.expected.txt"
    [ -f "$exp" ] || continue
    base="$(basename "$goo" .goo)"
    out_bin="build/golden_${base}.out"
    mkdir -p build
    if ! "$COMPILER" "$goo" -o "$out_bin" >/dev/null 2>"build/golden_${base}.cerr"; then
        echo "FAIL  $base (compile/link)"; fail=$((fail+1)); failed+=("$base"); continue
    fi
    envfile="${goo%.goo}.env"
    if [ -f "$envfile" ]; then
        # Source the fixture's env (KEY=VAL lines), exported only for this run.
        actual="$(set -a; . "$envfile"; set +a; "$out_bin" 2>/dev/null)"
    else
        actual="$("$out_bin" 2>/dev/null)"
    fi
    if [ "$actual" = "$(cat "$exp")" ]; then
        echo "PASS  $base"; pass=$((pass+1))
    else
        echo "FAIL  $base (output mismatch)"; diff <(printf '%s' "$(cat "$exp")") <(printf '%s' "$actual") | head -20
        fail=$((fail+1)); failed+=("$base")
    fi
done
echo "--- golden: $pass passed, $fail failed ---"
[ "$fail" -eq 0 ]
