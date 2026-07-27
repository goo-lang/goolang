#!/usr/bin/env bash
# Go spec conformance suite (tests/spec/). Manifest-driven: each row of
# tests/spec/manifest.tsv names a fixture, its enforced MODE, and its
# reported STATUS (see the manifest header for the vocabulary).
#
# A fixture is normally ONE file, tests/spec/<id>.goo. A `test`-mode fixture is
# instead a DIRECTORY, tests/spec/<id>/, run by `goo test`.
#
# The runner is a DRIFT GATE, not a scoreboard: a run-mode fixture that
# stops compiling / changes output, or a reject-mode fixture that starts
# compiling, fails the suite. Improving a construct therefore forces the
# matrix (and docs/GO_SPEC_CONFORMANCE.md) to be updated in the same
# change — claims can't rot silently in either direction.
#
# Exit statuses are captured directly off invocations (rc=$?), never
# through pipes, same discipline as run_golden.sh.
set -u
COMPILER="${COMPILER:-bin/goo}"
SPEC_DIR="${SPEC_DIR:-tests/spec}"
MANIFEST="$SPEC_DIR/manifest.tsv"
TIMEOUT_S="${SPEC_TIMEOUT:-10}"

# test-mode fixtures run with the package directory as their CWD, so the
# compiler path has to survive the cd. Every other mode runs from the repo
# root and is unaffected.
case "$COMPILER" in
  /*) COMPILER_ABS="$COMPILER" ;;
  *)  COMPILER_ABS="$PWD/$COMPILER" ;;
esac

[ -f "$MANIFEST" ] || { echo "spec-conformance: FAIL (missing $MANIFEST)"; exit 1; }
mkdir -p build/spec

pass=0; drift=0
declare -A status_count=()

# The exit status a fixture must produce: its .exit sidecar, or 0.
want_exit_for() {
    if [ -f "$SPEC_DIR/$1.exit" ]; then
        tr -d '[:space:]' < "$SPEC_DIR/$1.exit"
    else
        printf 0
    fi
}

# 0 when the captured stdout matches the fixture's .expected.txt.
matches_expected() {
    diff -q "$SPEC_DIR/$1.expected.txt" "build/spec/$1.stdout" >/dev/null 2>&1
}

while IFS=$'\t' read -r id mode status chapter note; do
    case "$id" in ''|'#'*) continue;; esac

    # test mode: the fixture is a DIRECTORY that `goo test` compiles and runs,
    # not a single file. `goo test` discovers TestXxx in the entry package's
    # _test files and synthesizes its main, so it cannot be expressed as a
    # one-file compile — hence a mode of its own rather than a row that claims
    # to cover it while testing something else.
    if [ "$mode" = "test" ]; then
        if [ ! -d "$SPEC_DIR/$id" ]; then
            echo "DRIFT $id (test-mode row has no fixture directory)"
            drift=$((drift+1)); continue
        fi
        # The redirection is applied by THIS shell, so the paths stay relative
        # to the repo root while the compiler runs inside the package.
        ( cd "$SPEC_DIR/$id" && timeout "$TIMEOUT_S" "$COMPILER_ABS" test . ) \
            >"build/spec/$id.stdout" 2>"build/spec/$id.cerr"
        rc=$?
        want_rc="$(want_exit_for "$id")"
        if [ "$rc" -ne "$want_rc" ]; then
            echo "DRIFT $id (exit $rc, want $want_rc: $(head -1 "build/spec/$id.cerr"))"
            drift=$((drift+1)); continue
        fi
        if ! matches_expected "$id"; then
            echo "DRIFT $id (output mismatch)"
            diff "$SPEC_DIR/$id.expected.txt" "build/spec/$id.stdout" | head -8
            drift=$((drift+1)); continue
        fi
        pass=$((pass+1)); status_count[$status]=$(( ${status_count[$status]:-0} + 1 )); continue
    fi

    goo="$SPEC_DIR/$id.goo"
    if [ ! -f "$goo" ]; then
        echo "DRIFT $id (manifest row has no fixture file)"; drift=$((drift+1)); continue
    fi
    bin="build/spec/$id"
    rm -f "$bin"
    "$COMPILER" "$goo" -o "$bin" >/dev/null 2>"build/spec/$id.cerr"
    crc=$?

    if [ "$mode" = "reject" ]; then
        if [ "$crc" -eq 0 ] || [ -e "$bin" ]; then
            echo "DRIFT $id (status '$status' expected a compile reject, but it now COMPILES — update the manifest/matrix)"
            drift=$((drift+1)); continue
        fi
        pass=$((pass+1)); status_count[$status]=$(( ${status_count[$status]:-0} + 1 )); continue
    fi

    # run mode
    if [ "$crc" -ne 0 ]; then
        echo "DRIFT $id (compile failed: $(head -1 "build/spec/$id.cerr"))"
        drift=$((drift+1)); continue
    fi
    timeout "$TIMEOUT_S" "$bin" >"build/spec/$id.stdout" 2>/dev/null
    rc=$?
    want_rc="$(want_exit_for "$id")"
    if [ "$rc" -ne "$want_rc" ]; then
        echo "DRIFT $id (exit $rc, want $want_rc)"; drift=$((drift+1)); continue
    fi
    if ! matches_expected "$id"; then
        echo "DRIFT $id (output mismatch)"
        diff "$SPEC_DIR/$id.expected.txt" "build/spec/$id.stdout" | head -8
        drift=$((drift+1)); continue
    fi
    pass=$((pass+1)); status_count[$status]=$(( ${status_count[$status]:-0} + 1 ))
done < "$MANIFEST"

total=$((pass + drift))
works=${status_count[works]:-0}
divergent=${status_count[divergent]:-0}
rejected=${status_count[rejected]:-0}
absent=${status_count[absent]:-0}
echo "--- spec-conformance: $pass/$total rows verified, $drift drifted ---"
echo "    works=$works divergent=$divergent rejected-by-decision=$rejected absent=$absent"
if [ "$pass" -gt 0 ] && [ "$drift" -eq 0 ]; then
    # Conformance = works / (everything except deliberate rejections).
    denom=$((works + divergent + absent))
    pct=$(( works * 100 / denom ))
    echo "    conformance (works / tested-excl-deliberate-rejections): ${pct}% (${works}/${denom})"
fi
[ "$drift" -eq 0 ]
