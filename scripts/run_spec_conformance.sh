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
# The doc gate checks the headline and the chapter table between their marker
# lines against the manifest. --write-doc regenerates both. --self-test proves
# the doc gate and the empty-manifest refusal can each report RED.
#
# Exit statuses are captured directly off invocations (rc=$?), never
# through pipes, same discipline as run_golden.sh.
set -u
COMPILER="${COMPILER:-bin/goo}"
SPEC_DIR="${SPEC_DIR:-tests/spec}"
MANIFEST="${SPEC_MANIFEST:-$SPEC_DIR/manifest.tsv}"
DOC="docs/GO_SPEC_CONFORMANCE.md"
TIMEOUT_S="${SPEC_TIMEOUT:-10}"
HB='<!-- spec-conformance:headline:begin -->'
HE='<!-- spec-conformance:headline:end -->'
TB='<!-- spec-conformance:table:begin -->'
TE='<!-- spec-conformance:table:end -->'

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

# Prints "works divergent rejected absent" counted from the manifest status column.
count_statuses() {
    awk -F'\t' '$1 ~ /^#/ || $1 == "" { next } { c[$3]++ }
        END { printf "%d %d %d %d\n", c["works"]+0, c["divergent"]+0, c["rejected"]+0, c["absent"]+0 }' "$1"
}

# A dash for a zero count, as the doc shows it.
cell() {
    if [ "$1" -eq 0 ]; then printf '–'; else printf '%s' "$1"; fi
}

# The prose of the Constructs column. It is hand-kept, so a new chapter needs a row here.
chapter_constructs() {
    case "$1" in
        Lexical) printf '%s' 'ASI/newlines, raw strings, rune literals, hex escapes' ;;
        Declarations) printf '%s' 'grouped var, const+iota, short var, named types, tuple assign, blank ident' ;;
        Types) printf '%s' 'arrays, slices, maps, struct embedding, pointers, closures, interfaces, channels, an interface embedded in a struct (dynamic promotion); **absent:** complex' ;;
        Expressions) printf '%s' 'keyed/elided composites, type assertions, method values, conversions, conversion to a named non-struct type, shift/div/mod, user variadics; **absent:** method expressions' ;;
        Statements) printf '%s' 'if-init, for (3 forms), for with init/condition/post OMITTED (all seven combinations), range (slices, string→**runes**, channels), switch-init + type switch, fallthrough, labels/goto, defer-in-loop, go+select, empty case/default bodies (switch/type-switch/select); **divergent:** named-result defer' ;;
        Builtins) printf '%s' 'len/cap/make/new/append/delete, copy, panic (exit 2), min/max (Go 1.21, NaN-propagating), clear (Go 1.21, map+slice); **rejected:** recover (clean v1 diagnostic)' ;;
        Generics) printf '%s' '`[T any]` inference-only funcs; explicit instantiation `f[T](...)` (single type param — Task C); **absent:** union constraints (P2.10), generic types' ;;
        Packages) printf '%s' 'shim-package alias imports (`import f "fmt"`, Task B); `io.Writer` with two implementations (`*os.File`, `bytes.Buffer`); multi-file packages (a package is the union of its files); `goo test` (discovers `TestXxx` in `_test` files, synthesizes the entry point, runs them); **rejected:** `init()` — clean diagnostic since the post-v1.0.0 fix (previously compiled but silently never ran)' ;;
        Errors) printf '%s' '(T, error) ↔ !T bridging, e.Error()' ;;
        System) printf '%s' '**rejected:** unsafe (v1 Non-goal)' ;;
        *) printf '' ;;
    esac
}

# Prints the chapter table: header, then one row per chapter in first-appearance order.
render_table() {
    printf '| Chapter | works | divergent | rejected | absent | Constructs |\n'
    printf '|---|---|---|---|---|---|\n'
    awk -F'\t' '$1 ~ /^#/ || $1 == "" { next }
        { if (!($4 in seen)) { seen[$4] = 1; order[++n] = $4 } cnt[$4 SUBSEP $3]++ }
        END { for (i = 1; i <= n; i++) { ch = order[i]
            printf "%s\t%d\t%d\t%d\t%d\n", ch, cnt[ch SUBSEP "works"] + 0, cnt[ch SUBSEP "divergent"] + 0, cnt[ch SUBSEP "rejected"] + 0, cnt[ch SUBSEP "absent"] + 0 } }' "$1" |
    while IFS=$'\t' read -r ch w d r a; do
        printf '| %s | %s | %s | %s | %s | %s |\n' "$ch" "$(cell "$w")" "$(cell "$d")" "$(cell "$r")" "$(cell "$a")" "$(chapter_constructs "$ch")"
    done
}

# Prints the one headline line. The date is today's date, and the checks ignore it.
render_headline() {
    local works="$1" divergent="$2" rejected="$3" absent="$4" denom pct
    denom=$((works + divergent + absent))
    [ "$denom" -gt 0 ] || return 1
    pct=$((works * 100 / denom))
    printf '**Snapshot:** %s, post-v1.0.0 — **%s%% construct coverage** (%s works / %s tested constructs, excluding %s deliberate rejections).\n' \
        "$(date +%F)" "$pct" "$works" "$denom" "$rejected"
}

strip_date() {
    sed -E 's/[0-9]{4}-[0-9]{2}-[0-9]{2}/DATE/g'
}

# Prints the lines between two marker lines, excluding the markers.
extract_between() {
    awk -v b="$2" -v e="$3" 'index($0, b) { f = 1; next } index($0, e) { f = 0; next } f' "$1"
}

# 0 when each marker occurs exactly once in the doc. Prints one FAIL line for each bad marker.
markers_ok() {
    local doc="$1" m n ok=1
    [ -f "$doc" ] || { echo "spec-conformance: FAIL (missing $doc)"; return 1; }
    for m in "$HB" "$HE" "$TB" "$TE"; do
        n="$(grep -cF -- "$m" "$doc" || true)"
        if [ "$n" -ne 1 ]; then
            echo "spec-conformance: FAIL (marker $m occurs $n times in $doc, want 1)"
            ok=0
        fi
    done
    [ "$ok" -eq 1 ]
}

# 0 when the doc headline and chapter table match the manifest counts.
check_doc() {
    local doc="$1" works="$2" divergent="$3" rejected="$4" absent="$5"
    local denom pct tmp have_h have_pct want_h fails=0
    markers_ok "$doc" || return 1
    denom=$((works + divergent + absent))
    [ "$denom" -gt 0 ] || { echo "spec-conformance: FAIL (no tested constructs to compute a percentage)"; return 1; }
    pct=$((works * 100 / denom))
    tmp="$(mktemp -d)" || { echo "spec-conformance: FAIL (mktemp failed)"; return 1; }

    have_h="$(extract_between "$doc" "$HB" "$HE" | strip_date)"
    have_pct="$(printf '%s\n' "$have_h" | sed -nE 's/.*\*\*([0-9]+)% construct coverage\*\*.*/\1/p')"
    if [ "$have_pct" != "$pct" ]; then
        echo "spec-conformance: FAIL (doc coverage percentage '${have_pct:-missing}' differs from computed ${pct}%)"
        fails=$((fails + 1))
    fi
    want_h="$(render_headline "$works" "$divergent" "$rejected" "$absent" | strip_date)"
    if [ "$have_h" != "$want_h" ]; then
        echo "spec-conformance: FAIL (doc headline differs from manifest: [$have_h] vs [$want_h])"
        fails=$((fails + 1))
    fi

    render_table "$MANIFEST" | strip_date > "$tmp/want_t"
    extract_between "$doc" "$TB" "$TE" | strip_date > "$tmp/have_t"
    if ! awk 'FILENAME == ARGV[1] { w[++nw] = $0; next } { h[++nh] = $0 }
        END { max = (nw > nh) ? nw : nh
            for (i = 1; i <= max; i++)
                if (w[i] != h[i]) {
                    printf "spec-conformance: FAIL (doc table line %d is [%s], manifest renders [%s])\n", i, h[i], w[i]
                    bad = 1
                }
            exit bad }' "$tmp/want_t" "$tmp/have_t"; then
        fails=$((fails + 1))
    fi
    rm -rf "$tmp"
    [ "$fails" -eq 0 ]
}

# Rewrites the text between the markers from the manifest counts, then checks the result.
write_doc() {
    local works="$1" divergent="$2" rejected="$3" absent="$4" tmp
    markers_ok "$DOC" || return 1
    tmp="$(mktemp -d)" || return 1
    render_headline "$works" "$divergent" "$rejected" "$absent" > "$tmp/head" || { rm -rf "$tmp"; return 1; }
    render_table "$MANIFEST" > "$tmp/table"
    if ! awk -v hb="$HB" -v he="$HE" -v tb="$TB" -v te="$TE" -v hf="$tmp/head" -v tf="$tmp/table" '
        function emit(f,   l) { while ((getline l < f) > 0) print l; close(f) }
        index($0, hb) { print; emit(hf); skip = 1; next }
        index($0, he) { skip = 0; print; next }
        index($0, tb) { print; emit(tf); skip = 1; next }
        index($0, te) { skip = 0; print; next }
        !skip { print }' "$DOC" > "$tmp/new.md"; then
        echo "spec-conformance: FAIL (could not rewrite $DOC)"
        rm -rf "$tmp"; return 1
    fi
    mv "$tmp/new.md" "$DOC"
    rm -rf "$tmp"
    check_doc "$DOC" "$works" "$divergent" "$rejected" "$absent"
}

# Each fault copies the doc, changes one thing, and must make check_doc return 1.
self_fault() {
    local name="$1" file="$2" rc
    check_doc "$file" "$works" "$divergent" "$rejected" "$absent" > "$file.log" 2>&1
    rc=$?
    if [ "$rc" -eq 1 ]; then
        echo "self-test fault $name: RED as expected"
        return 0
    fi
    echo "self-test fault $name: FAIL, expected RED (exit $rc)"
    return 1
}

# Refuses a fault copy that equals the doc, because such a copy proves nothing.
fault_changed() {
    if cmp -s "$DOC" "$1"; then
        echo "self-test: fault copy $1 equals the doc, the fault did not apply"
        return 1
    fi
}

self_test() {
    local tmp counts works divergent rejected absent ok=1 empty rc
    tmp="$(mktemp -d)" || { echo "spec-conformance --self-test: FAIL (mktemp failed)"; return 1; }
    counts="$(count_statuses "$MANIFEST")"
    read -r works divergent rejected absent <<< "$counts"

    cp "$DOC" "$tmp/control.md"
    if check_doc "$tmp/control.md" "$works" "$divergent" "$rejected" "$absent" > "$tmp/control.log" 2>&1; then
        echo "self-test control (unchanged doc): GREEN as expected"
    else
        echo "self-test control (unchanged doc): FAIL, expected GREEN"; ok=0
    fi

    sed -E 's/\*\*90% construct coverage\*\*/**91% construct coverage**/' "$DOC" > "$tmp/pct.md"
    if fault_changed "$tmp/pct.md" && self_fault "percentage changed by one" "$tmp/pct.md"; then :; else ok=0; fi

    sed -E 's/^\| Types \| 9 \|/| Types | 8 |/' "$DOC" > "$tmp/cell.md"
    if fault_changed "$tmp/cell.md" && self_fault "chapter cell changed by one" "$tmp/cell.md"; then :; else ok=0; fi

    sed '/spec-conformance:headline:begin/,/spec-conformance:headline:end/d' "$DOC" > "$tmp/nohead.md"
    if fault_changed "$tmp/nohead.md" && self_fault "headline marker block removed" "$tmp/nohead.md"; then :; else ok=0; fi

    { cat "$DOC"; printf '\n%s\n%s\n%s\n' "$HB" "$(render_headline "$works" "$divergent" "$rejected" "$absent")" "$HE"; } > "$tmp/twohead.md"
    if fault_changed "$tmp/twohead.md" && self_fault "second headline marker block added" "$tmp/twohead.md"; then :; else ok=0; fi

    { printf '%s\n\n' "$(render_headline "$works" "$divergent" "$rejected" "$absent")"; sed -E 's/\(50 works \/ 55/(51 works \/ 55/' "$DOC"; } > "$tmp/countchg.md"
    if fault_changed "$tmp/countchg.md" && self_fault "headline count changed, correct line outside markers" "$tmp/countchg.md"; then :; else ok=0; fi

    empty="$tmp/empty.tsv"
    : > "$empty"
    SPEC_MANIFEST="$empty" bash "$0" > "$tmp/empty.log" 2>&1
    rc=$?
    if [ "$rc" -eq 1 ] && grep -Fq 'no manifest rows were measured' "$tmp/empty.log"; then
        echo "self-test empty manifest: RED as expected"
    else
        echo "self-test empty manifest: FAIL, expected exit 1 (exit $rc)"; ok=0
    fi

    rm -rf "$tmp"
    if [ "$ok" -eq 1 ]; then
        echo "spec-conformance --self-test: PASS"
        return 0
    fi
    echo "spec-conformance --self-test: FAIL"
    return 1
}

WRITE_DOC=0
case "${1:-}" in
    --self-test) self_test; exit $? ;;
    --write-doc) WRITE_DOC=1 ;;
    "") ;;
    *) echo "spec-conformance: FAIL (unknown argument $1)"; exit 2 ;;
esac

COMPILER="${COMPILER:-bin/goo}"
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

# An empty manifest measures nothing, so it must not pass.
if [ "$pass" -eq 0 ]; then
    echo "spec-conformance: FAIL (no manifest rows were measured)"
    exit 1
fi

if [ "$drift" -eq 0 ]; then
    # Conformance = works / (everything except deliberate rejections).
    denom=$((works + divergent + absent))
    if [ "$denom" -eq 0 ]; then
        echo "spec-conformance: FAIL (no tested constructs to compute a percentage)"
        exit 1
    fi
    pct=$(( works * 100 / denom ))
    echo "    conformance (works / tested-excl-deliberate-rejections): ${pct}% (${works}/${denom})"
    # The headline and chapter table in the matrix are copies of these numbers.
    # A stale copy is a claim that rots, so this run refuses it, or rewrites it.
    if [ "$WRITE_DOC" -eq 1 ]; then
        write_doc "$works" "$divergent" "$rejected" "$absent" || drift=$((drift+1))
    else
        check_doc "$DOC" "$works" "$divergent" "$rejected" "$absent" || drift=$((drift+1))
    fi
fi
[ "$drift" -eq 0 ] && echo "spec-conformance: PASS"
[ "$drift" -eq 0 ]
