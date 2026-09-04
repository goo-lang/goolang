#!/usr/bin/env bash
# frontend_diff.sh — run two producers over the same fixtures and diff their
# dumps. A producer is a command that takes one fixture path and writes a dump
# to stdout. Reports "N same, M differ" and lists the differing fixtures;
# exit 1 if any differ. This is the gate every front-end swap is measured by:
# when producer B is the Haskell front end and M is 0 across every fixture,
# that component may flip.
#
#   scripts/frontend_diff.sh --a 'CMD' --b 'CMD' [--list FILE]
#   scripts/frontend_diff.sh --self-test
#
# Teeth: --self-test proves it reports a difference (b = sed rewrite) and
# reports none when the producers agree (a = b = cat).
set -u
cd "$(dirname "$0")/.."

self_test() {
    local tmp; tmp=$(mktemp -d)
    printf 'package main\n' > "$tmp/f1.goo"; printf 'package other\n' > "$tmp/f2.goo"
    printf '%s\n%s\n' "$tmp/f1.goo" "$tmp/f2.goo" > "$tmp/list"
    if ! out=$(bash "$0" --a cat --b cat --list "$tmp/list"); then echo "frontend-diff --self-test: FAIL (identical producers reported a difference)"; exit 1; fi
    if out=$(bash "$0" --a cat --b "sed s/other/changed/" --list "$tmp/list"); then echo "frontend-diff --self-test: FAIL (a rewritten fixture was not reported)"; exit 1; fi
    echo "$out" | grep -q 'f2.goo' || { echo "frontend-diff --self-test: FAIL (the differing fixture was not named)"; exit 1; }
    rm -rf "$tmp"
    echo "frontend-diff --self-test: PASS (2 same reports 0; 1 rewrite reports 1 and names it)"
}

A=""; B=""; LIST=""
while [ $# -gt 0 ]; do
    case "$1" in
        --self-test) self_test; exit 0 ;;
        --a) A=$2; shift 2 ;;
        --b) B=$2; shift 2 ;;
        --list) LIST=$2; shift 2 ;;
        *) echo "unknown argument: $1"; exit 2 ;;
    esac
done
[ -n "$A" ] && [ -n "$B" ] || { echo "usage: $0 --a CMD --b CMD [--list FILE]"; exit 2; }

fixtures() {
    if [ -n "$LIST" ]; then cat "$LIST"; else ls examples/*.expected.txt | sed 's/\.expected\.txt$/.goo/'; fi
}

tmp=$(mktemp -d); same=0; differ=0
while IFS= read -r f; do
    [ -z "$f" ] && continue
    $A "$f" > "$tmp/a" 2>/dev/null; $B "$f" > "$tmp/b" 2>/dev/null
    if cmp -s "$tmp/a" "$tmp/b"; then same=$((same+1)); else differ=$((differ+1)); echo "  DIFF  $f"; fi
done < <(fixtures)
rm -rf "$tmp"
echo "frontend-diff: $same same, $differ differ"
[ "$differ" -eq 0 ]
