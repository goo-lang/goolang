#!/usr/bin/env bash
# fixed-tmp-lint: no gate may write or read a literal /tmp/<name> path.
#
# WHY. Two sessions run gates on one machine, and a Bazel cache proof runs two
# suites at the same time on purpose. A fixed /tmp path is then one file with
# two writers. census-current-probe failed once and passed on rerun for this
# reason. Use mktemp (or mktemp -d) with a trap that removes it.
#
# A code line may keep a literal path if it ends with `# tmp-ok: <reason>`.
# A `# tmp-ok:` with no reason is itself a finding.
#
# Usage: fixed_tmp_lint.sh [--self-test] [FILE...]
#   no FILE: scan the Makefile and the .sh files under scripts/, tools/ and
#   tests/probes/ of the tree at FIXED_TMP_ROOT (default: the repo root).
# Exit: 0 clean, 1 findings, 2 tool failure (empty file list, unreadable file).
set -u

# Prints "file:line: text" for each finding in the files given as arguments.
scan() {
    awk '
        FNR == 1 { }
        /^[ \t]*#/ { next }                      # comment line
        /mktemp|TMPDIR/ { next }                 # the approved pattern
        {
            if (match($0, /\/tmp\/[A-Za-z0-9_]/) == 0) next
            if (match($0, /# tmp-ok:[ \t]*[^ \t]/)) next
            if ($0 ~ /# tmp-ok:/) { print FILENAME ":" FNR ": tmp-ok without a reason: " $0; next }
            print FILENAME ":" FNR ": " $0
        }
    ' "$@"
}

# The lint file holds the fixtures of its own self-test, so it is not scanned.
default_files() {
    local root="$1"
    {
        [ -f "$root/Makefile" ] && echo "$root/Makefile"
        for d in scripts tools tests/probes; do
            [ -d "$root/$d" ] && find "$root/$d" \( -type f -o -type l \) -name '*.sh' ! -name 'fixed_tmp_lint.sh'
        done
    } | LC_ALL=C sort
}

run() {
    local files=("$@")
    if [ "${#files[@]}" -eq 0 ]; then
        echo "fixed-tmp-lint: TOOL FAILURE empty file list" >&2
        return 2
    fi
    local f
    for f in "${files[@]}"; do
        [ -r "$f" ] || { echo "fixed-tmp-lint: TOOL FAILURE cannot read $f" >&2; return 2; }
    done
    local out
    out=$(scan "${files[@]}") || { echo "fixed-tmp-lint: TOOL FAILURE awk failed" >&2; return 2; }
    if [ -n "$out" ]; then
        printf '%s\n' "$out"
        echo "fixed-tmp-lint: FAIL $(printf '%s\n' "$out" | grep -c .) fixed /tmp path(s); use mktemp, or end the line with '# tmp-ok: <reason>'"
        return 1
    fi
    echo "fixed-tmp-lint: PASS ${#files[@]} files, no fixed /tmp path"
    return 0
}

self_test() {
    # The result must not depend on the machine: fixed PATH, fixtures only.
    PATH=/usr/bin:/bin
    export PATH
    local w fail=0
    w=$(mktemp -d) || { echo "fixed-tmp-lint-selftest: TOOL FAILURE mktemp" >&2; return 2; }
    trap 'rm -rf "$w"' RETURN
    local P=/tmp/ # fixtures are built from this, so no source line holds a path

    expect() { # name want_rc file
        local name="$1" want="$2" file="$3" rc
        FIXED_TMP_ROOT="$w/empty" "$0" "$file" >/dev/null 2>&1
        rc=$?
        if [ "$rc" -ne "$want" ]; then
            echo "fixed-tmp-lint-selftest: FAIL $name (rc=$rc want $want)"
            fail=1
        fi
    }
    printf 'cat x > %sfoo.out\n'                      "$P" > "$w/literal.sh"
    printf 'cat x > %sfoo.out # tmp-ok: container-private\n' "$P" > "$w/ok.sh"
    printf 'cat x > %sfoo.out # tmp-ok:\n'            "$P" > "$w/noreason.sh"
    printf 'cat x > %sfoo.out # tmp-ok:   \n'         "$P" > "$w/blankreason.sh"
    printf '  # writes %sfoo.out\n'                   "$P" > "$w/comment.sh"
    printf 'w=$(mktemp %sfoo.XXXXXX)\n'               "$P" > "$w/mktemp.sh"
    printf 'w="${TMPDIR:-/tmp}/foo"\n'                      > "$w/tmpdir.sh"
    printf '\tcat x > %sfoo.out\n'                    "$P" > "$w/Makefile"

    expect "literal path is reported"            1 "$w/literal.sh"
    expect "tmp-ok with reason is accepted"      0 "$w/ok.sh"
    expect "tmp-ok with no reason is reported"   1 "$w/noreason.sh"
    expect "tmp-ok with blank reason is reported" 1 "$w/blankreason.sh"
    expect "comment line is not reported"        0 "$w/comment.sh"
    expect "mktemp line is not reported"         0 "$w/mktemp.sh"
    expect "TMPDIR line is not reported"         0 "$w/tmpdir.sh"
    expect "Makefile recipe is reported"         1 "$w/Makefile"
    mkdir "$w/empty"
    FIXED_TMP_ROOT="$w/empty" "$0" >/dev/null 2>&1
    [ $? -eq 2 ] || { echo "fixed-tmp-lint-selftest: FAIL empty list must exit 2"; fail=1; }
    "$0" "$w/nonexistent.sh" >/dev/null 2>&1
    [ $? -eq 2 ] || { echo "fixed-tmp-lint-selftest: FAIL unreadable file must exit 2"; fail=1; }

    if [ "$fail" -ne 0 ]; then return 1; fi
    echo "fixed-tmp-lint-selftest: PASS 10 cases"
}

if [ "${1:-}" = "--self-test" ]; then
    self_test
    exit $?
fi

if [ "$#" -gt 0 ]; then
    run "$@"
    exit $?
fi
# Under bazel test, $0 is the test launcher in tests/probes/, not this file's
# place in the tree, so the runfiles root is the one that holds the data.
if [ -z "${FIXED_TMP_ROOT:-}" ] && [ -n "${TEST_SRCDIR:-}" ] && [ -n "${TEST_WORKSPACE:-}" ]; then
    FIXED_TMP_ROOT="$TEST_SRCDIR/$TEST_WORKSPACE"
fi
root="${FIXED_TMP_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
mapfile -t files < <(default_files "$root")
run "${files[@]}"
exit $?
