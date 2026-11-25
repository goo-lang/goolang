#!/usr/bin/env bash
# superset-probe: a grow-only ratchet for "all valid Go is valid Goo".
#
# Measures how much of the pinned Go test corpus bin/goo accepts at three levels:
#   L1 parse      bin/goo --emit-ast-json P exits 0
#   L2 compile    bin/goo -o <tmp>/x P exits 0                  (20 s timeout)
#   L3 behaviour  binary exits 0, stdin /dev/null, stdout equals P.out when
#                 that file exists                              (10 s timeout)
# Ratchet files tests/superset/pass-L{1,2,3}.txt list the programs that passed.
# Check mode fails when a listed program stops passing. A new pass stays green.
#
# Usage: superset_probe.sh [--update | --self-test]
# Env (used by --self-test): SUPERSET_CORPUS, SUPERSET_RATCHET_DIR, GOO_BIN.
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# The compiler passes the relative path lib/libgoo_runtime.a to the linker, so it
# links only from the repository root. This is a known compiler limit.
cd "$ROOT" || exit 2
export GOOROOT="$ROOT"
CORPUS=${SUPERSET_CORPUS:-$ROOT/third_party/go-test-corpus/go1.27.4}
RATCHET=${SUPERSET_RATCHET_DIR:-$ROOT/tests/superset}
GOO=${GOO_BIN:-$ROOT/bin/goo}
export LC_ALL=C

# --one P: print "<rel> <l1> <l2> <l3>" for one program. Runs inside xargs.
if [ "${1:-}" = "--one" ]; then
  p=$2
  rel=${p#"$CORPUS"/}
  l1=0 l2=0 l3=0
  w=$(mktemp -d "${TMPDIR:-/tmp}/superset.XXXXXX") || exit 2
  if timeout 20 "$GOO" --emit-ast-json "$p" >/dev/null 2>&1; then
    l1=1
  fi
  if timeout 20 "$GOO" -o "$w/prog" "$p" >/dev/null 2>&1; then
    l2=1
    out="${p%.go}.out"
    # The group silences the shell's "Floating point exception" job message.
    if { (cd "$w" && timeout 10 ./prog </dev/null >"$w/stdout" 2>/dev/null); } 2>/dev/null; then
      if [ ! -f "$out" ] || cmp -s "$w/stdout" "$out"; then l3=1; fi
    fi
  fi
  rm -rf "$w"
  echo "$rel $l1 $l2 $l3"
  exit 0
fi

# run_all: fill $RES with one line per program, sorted.
run_all() {
  RES=$(mktemp "${TMPDIR:-/tmp}/superset-res.XXXXXX") || return 2
  if [ ! -d "$CORPUS" ]; then
    echo "superset-probe: FAIL (corpus folder missing: $CORPUS)"; return 1
  fi
  find "$CORPUS" -name '*.go' | LC_ALL=C sort >"$RES.list"
  if [ ! -s "$RES.list" ]; then
    echo "superset-probe: FAIL (corpus holds no .go file: $CORPUS)"; return 1
  fi
  if [ ! -x "$GOO" ]; then
    echo "superset-probe: FAIL (compiler missing: $GOO)"; return 1
  fi
  export CORPUS GOO GOOROOT
  xargs -P "$(nproc)" -n 1 -a "$RES.list" "${BASH_SOURCE[0]}" --one | LC_ALL=C sort >"$RES"
  want=$(wc -l <"$RES.list")
  got=$(wc -l <"$RES")
  rm -f "$RES.list"
  if [ "$got" -ne "$want" ] || [ "$got" -eq 0 ]; then
    echo "superset-probe: FAIL (result list has $got lines for $want programs)"; return 1
  fi
  return 0
}

# level_list N: paths that pass level N (1-3), sorted.
level_list() { awk -v c=$(( $1 + 1 )) '$c == 1 { print $1 }' "$RES" | LC_ALL=C sort; }

do_check() {
  run_all || return 1
  total=$(wc -l <"$RES")
  fail=0
  declare -a counts
  for n in 1 2 3; do
    cur=$(mktemp "${TMPDIR:-/tmp}/superset-cur.XXXXXX")
    level_list "$n" >"$cur"
    counts[n]=$(wc -l <"$cur")
    f="$RATCHET/pass-L$n.txt"
    if [ ! -f "$f" ]; then
      echo "superset-probe: FAIL (L$n: ratchet file missing: $f)"; fail=1
    else
      while IFS= read -r gone; do
        echo "superset-probe: FAIL (L$n: $gone no longer passes)"; fail=1
      done < <(LC_ALL=C comm -23 "$f" "$cur")
      newc=$(LC_ALL=C comm -13 "$f" "$cur" | wc -l)
      if [ "$newc" -gt 0 ]; then
        LC_ALL=C comm -13 "$f" "$cur" | sed "s/^/superset-probe: new L$n pass: /"
        echo "superset-probe: $newc new L$n pass(es); run --update to record them"
      fi
    fi
    rm -f "$cur"
  done
  # An instrument that sees no parse pass reads nothing; it must not pass.
  if [ "${counts[1]}" -eq 0 ]; then
    echo "superset-probe: FAIL (no program passes L1: the instrument is broken)"; fail=1
  fi
  rm -f "$RES"
  [ "$fail" -eq 0 ] || return 1
  echo "superset-probe: PASS (L1 ${counts[1]}/$total, L2 ${counts[2]}/$total, L3 ${counts[3]}/$total of the go1.27.4 run corpus)"
}

do_update() {
  run_all || return 1
  mkdir -p "$RATCHET"
  for n in 1 2 3; do level_list "$n" >"$RATCHET/pass-L$n.txt"; done
  echo "superset-probe: updated $RATCHET ($(wc -l <"$RATCHET/pass-L1.txt") / $(wc -l <"$RATCHET/pass-L2.txt") / $(wc -l <"$RATCHET/pass-L3.txt") of $(wc -l <"$RES"))"
  rm -f "$RES"
}

do_selftest() {
  # Scrub inherited state so the machine cannot change the verdict.
  T=$(mktemp -d "${TMPDIR:-/tmp}/superset-self.XXXXXX")
  trap 'rm -rf "$T"' EXIT
  self=${BASH_SOURCE[0]}
  mkdir -p "$T/c" "$T/r"
  printf '%s\n' 'package main' '' 'func main() { println("hi") }' >"$T/c/good.go"
  printf '%s\n' 'package main' '' 'import "fmt"' '' 'func main() { fmt.Print("hi\n") }' >"$T/c/out.go"
  printf 'hi\n' >"$T/c/out.out"
  printf '%s\n' 'package main' '' 'func main() { println("x" }' >"$T/c/bad.go"
  run() { env -i PATH="$PATH" HOME="${HOME:-/}" TMPDIR="$T" SUPERSET_CORPUS="${C:-$T/c}" \
          SUPERSET_RATCHET_DIR="${R:-$T/r}" GOO_BIN="${G:-$GOO}" bash "$ROOT/scripts/superset_probe.sh" "$@"; }
  bad=0
  expect() { # name want_rc pattern output rc
    if [ "$2" = 0 ] && [ "$5" -ne 0 ]; then echo "superset-selftest: FAIL ($1: rc=$5, want 0)"; bad=1; return; fi
    if [ "$2" != 0 ] && [ "$5" -eq 0 ]; then echo "superset-selftest: FAIL ($1: rc=0, want red)"; bad=1; return; fi
    if [ -n "$3" ] && ! grep -qF -- "$3" <<<"$4"; then echo "superset-selftest: FAIL ($1: output lacks '$3'): $4"; bad=1; return; fi
    echo "superset-selftest: ok ($1)"
  }
  # (a) control: update on the tiny corpus, then check is green, and bad.go passes nothing.
  o=$(run --update); rc=$?; expect "control update" 0 "updated" "$o" $rc
  if grep -qx 'bad.go' "$T/r/pass-L1.txt"; then echo "superset-selftest: FAIL (bad.go passed L1)"; bad=1; fi
  grep -qx 'good.go' "$T/r/pass-L3.txt" || { echo "superset-selftest: FAIL (good.go missing from L3 control)"; bad=1; }
  grep -qx 'out.go' "$T/r/pass-L3.txt" || { echo "superset-selftest: FAIL (out.go missing from L3 control)"; bad=1; }
  o=$(run); rc=$?; expect "control check green" 0 "superset-probe: PASS (L1 2/3, L2 2/3, L3 2/3" "$o" $rc
  # (b1) L1: ratchet lists a program with a syntax error.
  cp "$T/r/pass-L1.txt" "$T/r1.bak"; echo bad.go >>"$T/r/pass-L1.txt"; LC_ALL=C sort -o "$T/r/pass-L1.txt" "$T/r/pass-L1.txt"
  o=$(run); rc=$?; expect "L1 red" 1 "FAIL (L1: bad.go no longer passes)" "$o" $rc
  cp "$T/r1.bak" "$T/r/pass-L1.txt"
  # (b2) L3: the expected output differs, so only L3 goes red.
  printf 'different\n' >"$T/c/out.out"
  o=$(run); rc=$?; expect "L3 red" 1 "FAIL (L3: out.go no longer passes)" "$o" $rc
  if grep -qF "FAIL (L1:" <<<"$o" || grep -qF "FAIL (L2:" <<<"$o"; then echo "superset-selftest: FAIL (L3 break leaked to L1/L2)"; bad=1; fi
  printf 'hi\n' >"$T/c/out.out"
  # (b3) L2: a program that parses but does not type check.
  printf '%s\n' 'package main' '' 'func main() { var x int = "s"; _ = x }' >"$T/c/tc.go"
  echo tc.go >>"$T/r/pass-L2.txt"; LC_ALL=C sort -o "$T/r/pass-L2.txt" "$T/r/pass-L2.txt"
  o=$(run); rc=$?; expect "L2 red" 1 "FAIL (L2: tc.go no longer passes)" "$o" $rc
  rm "$T/c/tc.go"
  # (c1) empty corpus.
  mkdir -p "$T/empty"; o=$(C="$T/empty" run); rc=$?; expect "empty corpus red" 1 "holds no .go file" "$o" $rc
  # (c2) missing corpus.
  o=$(C="$T/none" run); rc=$?; expect "missing corpus red" 1 "corpus folder missing" "$o" $rc
  # (c3) a compiler that fails every program leaves no L1 pass: red, even with empty ratchets.
  mkdir -p "$T/r0"; : >"$T/r0/pass-L1.txt"; : >"$T/r0/pass-L2.txt"; : >"$T/r0/pass-L3.txt"
  o=$(R="$T/r0" G=/bin/false run); rc=$?; expect "dead compiler red" 1 "instrument is broken" "$o" $rc
  # (c4) a missing compiler.
  o=$(G="$T/nocompiler" run); rc=$?; expect "missing compiler red" 1 "compiler missing" "$o" $rc
  [ "$bad" -eq 0 ] || { echo "superset-selftest: FAIL"; return 1; }
  echo "superset-selftest: PASS"
}

case "${1:-}" in
  "")          do_check ;;
  --update)    do_update ;;
  --self-test) do_selftest ;;
  *) echo "usage: $0 [--update | --self-test]" >&2; exit 2 ;;
esac
