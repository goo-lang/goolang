#!/usr/bin/env bash
# Go spec conformance suite (tests/spec/). Manifest-driven: each row of
# tests/spec/manifest.tsv names a fixture, its enforced MODE, and its
# reported STATUS (see the manifest header for the vocabulary).
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

[ -f "$MANIFEST" ] || { echo "spec-conformance: FAIL (missing $MANIFEST)"; exit 1; }
mkdir -p build/spec

pass=0; drift=0
declare -A status_count=()

while IFS=$'\t' read -r id mode status chapter note; do
    case "$id" in ''|'#'*) continue;; esac
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
    want_rc=0
    if [ -f "$SPEC_DIR/$id.exit" ]; then
        want_rc="$(tr -d '[:space:]' < "$SPEC_DIR/$id.exit")"
    fi
    if [ "$rc" -ne "$want_rc" ]; then
        echo "DRIFT $id (exit $rc, want $want_rc)"; drift=$((drift+1)); continue
    fi
    if ! diff -q "$SPEC_DIR/$id.expected.txt" "build/spec/$id.stdout" >/dev/null 2>&1; then
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
