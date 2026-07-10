#!/usr/bin/env bash
# P5.4: CLI exit-code and stderr discipline — table-driven audit of bin/goo.
#
# Contract under test (docs/2026-07-08-v1-roadmap.md P5.4):
#   success = exit 0, parse error = 1, type error = 1, link failure nonzero,
#   run-failure propagation, ALL error text on stderr (stdout carries only
#   the program's requested output: compiled-program stdout, `goo help`).
#
# Runs in `make test` (target test-cli). Each row asserts three channels at
# once: exit code, exact-or-contains stdout, and a stderr mode (empty /
# nonempty / substring) — a regression on any one stream fails the row.
set -u

COMPILER=${1:?usage: cli_test.sh <path-to-goo>}
case "$COMPILER" in
  /*) ;;
  *) COMPILER="$(cd "$(dirname "$COMPILER")" && pwd)/$(basename "$COMPILER")" ;;
esac

WORK=build/cli_tests
rm -rf "$WORK"
mkdir -p "$WORK"

pass=0
fail=0

cat > "$WORK/ok.goo" <<'EOF'
package main

import "fmt"

func main() {
	fmt.Println("ok")
}
EOF

cat > "$WORK/parse_err.goo" <<'EOF'
package main

func main() { if { {
EOF

cat > "$WORK/type_err.goo" <<'EOF'
package main

import "fmt"

func main() {
	var x int = "not an int"
	fmt.Println(x)
}
EOF

cat > "$WORK/exit5.goo" <<'EOF'
package main

import "os"

func main() {
	os.Exit(5)
}
EOF

# check <name> <want_exit> <want_stdout> <stderr_mode> <cmd...>
#   want_stdout: exact string, or "~substr" for contains
#   stderr_mode: "empty", "nonempty", or a substring stderr must contain
check() {
  local name="$1" want_exit="$2" want_stdout="$3" stderr_mode="$4"
  shift 4
  local out rc err bad=""
  out=$("$@" 2>"$WORK/stderr.txt")
  rc=$?
  err=$(cat "$WORK/stderr.txt")

  if [ "$rc" -ne "$want_exit" ]; then
    bad="exit=$rc want=$want_exit"
  fi
  case "$want_stdout" in
    "~"*)
      case "$out" in
        *"${want_stdout#\~}"*) ;;
        *) bad="$bad; stdout missing '${want_stdout#\~}'" ;;
      esac
      ;;
    *)
      if [ "$out" != "$want_stdout" ]; then
        bad="$bad; stdout='$out' want='$want_stdout'"
      fi
      ;;
  esac
  case "$stderr_mode" in
    empty)
      if [ -n "$err" ]; then bad="$bad; stderr not empty: '$(printf '%s' "$err" | head -c 100)'"; fi
      ;;
    nonempty)
      if [ -z "$err" ]; then bad="$bad; stderr empty (error text lost or on stdout)"; fi
      ;;
    *)
      case "$err" in
        *"$stderr_mode"*) ;;
        *) bad="$bad; stderr missing '$stderr_mode': got '$(printf '%s' "$err" | head -c 100)'" ;;
      esac
      ;;
  esac

  if [ -z "$bad" ]; then
    echo "  PASS $name"
    pass=$((pass + 1))
  else
    echo "  FAIL $name: ${bad#; }"
    fail=$((fail + 1))
  fi
}

echo "=== cli_test: exit-code and stderr discipline (P5.4) ==="

#     name                    exit  stdout        stderr-mode          command
check compile-success         0     ""            empty                "$COMPILER" "$WORK/ok.goo" -o "$WORK/ok_bin"
check compiled-binary-runs    0     "ok"          empty                "$WORK/ok_bin"
check run-success             0     "ok"          empty                "$COMPILER" run "$WORK/ok.goo"
check run-exit-propagation    5     ""            empty                "$COMPILER" run "$WORK/exit5.goo"
check parse-error             1     ""            "Parse error"        "$COMPILER" "$WORK/parse_err.goo" -o "$WORK/pe_bin"
check type-error              1     ""            "Type error"         "$COMPILER" "$WORK/type_err.goo" -o "$WORK/te_bin"
check missing-input           1     ""            "Cannot open file"   "$COMPILER" "$WORK/does_not_exist.goo" -o "$WORK/mi_bin"
check no-input-file           1     ""            "No input file"      "$COMPILER"
check unknown-flag            1     ""            nonempty             "$COMPILER" --totally-bogus-flag "$WORK/ok.goo"
check link-failure            1     ""            "-lgootest_bogus_xyz" "$COMPILER" "$WORK/ok.goo" -l gootest_bogus_xyz -o "$WORK/lf_bin"
check build-extra-arg         1     ""            "unexpected argument" "$COMPILER" build "$WORK/ok.goo" stray_arg
check run-emit-llvm-conflict  1     ""            "cannot be combined" "$COMPILER" run --emit-llvm "$WORK/ok.goo"
check help-on-stdout          0     "~Usage:"     empty                "$COMPILER" help

echo "--- cli_test: $pass passed, $fail failed ---"
[ "$fail" -eq 0 ]
