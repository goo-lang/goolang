#!/usr/bin/env bash
# vendor-drift-probe (gates T-3 and T-8 of the trusted-stdlib design).
#
# Every vendored file under goostd/<pkg>/ must equal its pristine twin under
# third_party/go1.27.4-src/<pkg>/ after the ledger goostd/VENDOR.tsv is applied
# to the pristine bytes. Needs no Go toolchain, so it runs in CI.
#
# Ledger columns (tab-separated, no header, sorted with LC_ALL=C):
#   package  file  kind  detail
#   rename    old->new@line:col   (a Goo keyword used as an identifier)
#   swap      asm_file->generic_file
#   drop      file.s
#   override  reason
#
# VENDOR_ROOT (env) points the probe at another tree. The self-test uses it.
# --self-test builds each fault in a temporary tree, checks that the fault
# changed the input, and requires the probe to FAIL for the named reason.
set -u
export LC_ALL=C
self="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

run_probe() {
    # PROBE-BODY-START
    echo "vendor-drift-probe: FAIL[NOT-IMPLEMENTED] probe body missing"
    return 2
    # PROBE-BODY-END
}

tree_hash() {
    (cd "$1" && find . -type f -print0 | sort -z | xargs -0 -r sha256sum | sha256sum | cut -d' ' -f1)
}

make_synth_tree() {
    local t="$1"
    mkdir -p "$t/goostd/synth/plain" "$t/goostd/synth/renamed" \
        "$t/third_party/go1.27.4-src/synth/plain" "$t/third_party/go1.27.4-src/synth/renamed"
    printf 'package plain\n\nfunc F() int { return 1 }\n' > "$t/third_party/go1.27.4-src/synth/plain/plain.go"
    cp "$t/third_party/go1.27.4-src/synth/plain/plain.go" "$t/goostd/synth/plain/plain.go"
    printf 'package renamed\n\nfunc G(match int) int {\n\t// match stays in a comment\n\treturn match + 1\n}\n' \
        > "$t/third_party/go1.27.4-src/synth/renamed/r.go"
    printf 'package renamed\n\nfunc G(match_ int) int {\n\t// match stays in a comment\n\treturn match_ + 1\n}\n' \
        > "$t/goostd/synth/renamed/r.go"
    printf 'synth/renamed\tr.go\trename\tmatch->match_@3:8\nsynth/renamed\tr.go\trename\tmatch->match_@5:9\n' \
        > "$t/goostd/VENDOR.tsv"
}

self_test() {
    local fails=0 before after out rc
    base=""; work=""
    base="$(mktemp -d)"; make_synth_tree "$base"
    trap 'rm -rf "$base" "$work"' EXIT

    # expect_green <label> <root>
    expect_green() {
        out="$(VENDOR_ROOT="$2" bash "$self" 2>&1)"; rc=$?
        if [ $rc -eq 0 ]; then echo "  ok   $1: GREEN"; else echo "  FAIL $1: expected GREEN, got rc=$rc: $(echo "$out" | tail -1)"; fails=$((fails+1)); fi
    }
    # fault <label> <expected tag> <mutation function>
    fault() {
        local label="$1" tag="$2" mut="$3"
        work="$(mktemp -d)"; cp -a "$base/." "$work/"
        before="$(tree_hash "$work")"
        "$mut" "$work"
        after="$(tree_hash "$work")"
        if [ "$before" = "$after" ]; then
            echo "  FAIL $label: the mutation did not change the input"; fails=$((fails+1)); rm -rf "$work"; return
        fi
        out="$(VENDOR_ROOT="$work" bash "$self" 2>&1)"; rc=$?
        if [ $rc -eq 1 ] && echo "$out" | grep -q "FAIL\[$tag\]"; then
            echo "  ok   $label: RED ($(echo "$out" | grep -m1 "FAIL\[$tag\]"))"
        else
            echo "  FAIL $label: expected RED with $tag, got rc=$rc: $(echo "$out" | tail -1)"; fails=$((fails+1))
        fi
        rm -rf "$work"
    }
    m_byte()      { printf ' ' >> "$1/goostd/synth/plain/plain.go"; }
    m_rowcol()    { sed -i 's/@5:9/@5:10/' "$1/goostd/VENDOR.tsv"; }
    m_unrenamed() { cp "$1/third_party/go1.27.4-src/synth/renamed/r.go" "$1/goostd/synth/renamed/r.go"; }
    m_nopristine(){ rm "$1/third_party/go1.27.4-src/synth/plain/plain.go"; }
    m_novendored(){ rm "$1/goostd/synth/plain/plain.go"; }
    m_extra()     { printf 'package plain\n' > "$1/goostd/synth/plain/extra.go"; }
    m_empty()     { rm -rf "$1/third_party/go1.27.4-src/synth" "$1/goostd/synth"; : > "$1/goostd/VENDOR.tsv"; }

    echo "vendor-drift-probe --self-test"
    expect_green "control (synthetic tree, one rename pair)" "$base"
    expect_green "control (unchanged repository tree)" "$repo"
    fault "one changed byte, no ledger row"           DRIFT           m_byte
    fault "rename row with no matching rename (col)"  RENAME          m_rowcol
    fault "rename row, vendored file not renamed"     DRIFT           m_unrenamed
    fault "pristine file missing"                     NO-PRISTINE     m_nopristine
    fault "vendored twin missing"                     NO-VENDORED     m_novendored
    fault "vendored file with no pristine twin"       NO-PRISTINE-TWIN m_extra
    fault "empty work list"                           EMPTY           m_empty
    if [ $fails -eq 0 ]; then echo "vendor-drift-probe --self-test: PASS"; return 0; fi
    echo "vendor-drift-probe --self-test: FAIL ($fails)"; return 1
}

if [ "${1:-}" = "--self-test" ]; then self_test; exit $?; fi
VENDOR_ROOT="${VENDOR_ROOT:-$repo}" run_probe
