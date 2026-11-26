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

# Parse error where the bridge has read ahead: the rejected token is the `}`
# on line 5, column 1. The old text reported the lexer position instead.
printf 'package main\n\nfunc main() {\n\tx := 1 +\n}\n' > "$WORK/parse_err_tok.goo"

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

# A DIRECTORY entry package: `goo build .` compiles every source file in the
# directory as one package (Go's unit of compilation). Two files, with main.go
# calling a function declared in the sibling file, so the case fails if the
# files are compiled separately rather than as one package.
mkdir -p "$WORK/dirpkg"
cat > "$WORK/dirpkg/main.go" <<'EOF'
package main

import "fmt"

func main() {
	fmt.Println(greeting(), answer())
}
EOF
cat > "$WORK/dirpkg/helper.go" <<'EOF'
package main

func greeting() string {
	return "hello"
}

func answer() int {
	return 42
}
EOF

# A directory with no buildable source files must be refused cleanly.
mkdir -p "$WORK/emptydir"

# A package with no _test files: `goo test` reports Go's no-tests line, exit 0.
mkdir -p "$WORK/notests"
cat > "$WORK/notests/only.goo" <<'EOF'
package notests

func Add(a int, b int) int { return a + b }
EOF

# A Test-named function of the wrong shape is a COMPILE ERROR under `goo test`,
# never a silent skip — a skipped test reports success, which is worse than
# having no test at all. The reject-fixture harness cannot express this case:
# run_golden_reject.sh compiles in legacy mode, and discovery deliberately does
# not run for `goo build`, so a fixture there would compile clean and the suite
# would report a false failure. Hence a CLI row.
mkdir -p "$WORK/badsig"
cat > "$WORK/badsig/x_test.goo" <<'EOF'
package badsig

import "testing"

func TestWrongShape(a int, b int) int {
	return a + b
}

func main() {
	testing.Summary()
}
EOF

# The companion positive case. Without it, a discovery bug that rejects EVERY
# Test function still satisfies the negative row above.
#
# No main: the compiler synthesizes _testmain.goo. A hand-written main here
# would now collide with the generated one.
mkdir -p "$WORK/goodsig"
cat > "$WORK/goodsig/y_test.goo" <<'EOF'
package goodsig

import "testing"

func TestFine(t *testing.T) {
}
EOF

# A FAILING test. This is the row that proves a discovered test was actually
# CALLED: a passing run prints only `ok`, which a synthesized main that called
# nothing would also print. The --- FAIL header names the test, so it can only
# appear if testing.Run("TestBoom", TestBoom) was generated and executed.
mkdir -p "$WORK/failpkg"
cat > "$WORK/failpkg/boom_test.goo" <<'EOF'
package failpkg

import "testing"

func TestBoom(t *testing.T) {
	t.Errorf("boom %d", 42)
}
EOF

# A `package main` PROGRAM with tests. Every CLI tool is package main, so if
# this cannot be tested, no CLI tool can be. The synthesized _testmain.goo used
# to collide with the program's own main ("Function 'main' already declared").
mkdir -p "$WORK/mainpkg"
cat > "$WORK/mainpkg/tool.goo" <<'EOF'
package main

import "fmt"

func double(x int) int { return x * 2 }

func main() {
	fmt.Println(double(21))
}
EOF
cat > "$WORK/mainpkg/tool_test.goo" <<'EOF'
package main

import "testing"

func TestDouble(t *testing.T) {
	if double(21) != 42 {
		t.Errorf("double(21) = %d, want 42", double(21))
	}
}
EOF

# A _test file that declares NO test functions is not an error: the synthesized
# main just summarizes an empty run, the way `go test` does.
mkdir -p "$WORK/emptytests"
cat > "$WORK/emptytests/e_test.goo" <<'EOF'
package emptytests

func helper() int { return 1 }
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

# Diagnostic snippets (Review Focus 3 and 5). The type error sits in the SECOND
# file of a package directory, so the snippet must come from that file's text
# and not from the first one registered. A "./x.goo" spelling must render the
# snippet too, because the registry keys on the exact filename string.
mkdir -p "$WORK/twofile"
cat > "$WORK/twofile/a.go" <<'EOF2'
package main

func main() {
	helper()
}
EOF2
cat > "$WORK/twofile/b.go" <<'EOF2'
package main

func helper() {
	var second int = "in second file"
	_ = second
}
EOF2
cp "$WORK/type_err.goo" "$WORK/dotslash.goo"

echo "=== cli_test: exit-code and stderr discipline (P5.4) ==="

#     name                    exit  stdout        stderr-mode          command
check compile-success         0     ""            empty                "$COMPILER" "$WORK/ok.goo" -o "$WORK/ok_bin"
check compiled-binary-runs    0     "ok"          empty                "$WORK/ok_bin"
check run-success             0     "ok"          empty                "$COMPILER" run "$WORK/ok.goo"
check run-exit-propagation    5     ""            empty                "$COMPILER" run "$WORK/exit5.goo"
check parse-error             1     ""            ": error: syntax error" "$COMPILER" "$WORK/parse_err.goo" -o "$WORK/pe_bin"
check parse-error-names-token 1     ""            ":5:1: error: syntax error, unexpected '}'" "$COMPILER" "$WORK/parse_err_tok.goo" -o "$WORK/pet_bin"
check parse-error-no-trailer  0     ""            empty                sh -c "! '$COMPILER' '$WORK/parse_err_tok.goo' -o '$WORK/pet_bin' 2>&1 | grep -q 'Parse failed'"
check type-error              1     ""            ": error: Cannot assign string to int64"        "$COMPILER" "$WORK/type_err.goo" -o "$WORK/te_bin"
check snippet-on-type-error   1     ""            " | 	var x int = \"not an int\"" "$COMPILER" "$WORK/type_err.goo" -o "$WORK/te_bin"
check missing-input           1     ""            "Cannot open file"   "$COMPILER" "$WORK/does_not_exist.goo" -o "$WORK/mi_bin"
check no-input-file           1     ""            "No input file"      "$COMPILER"
check no-position-error-form  0     ""            empty                sh -c "'$COMPILER' '$WORK/does_not_exist.goo' -o '$WORK/mi_bin' 2>&1 >/dev/null | head -n 1 | grep -q '^goo: error: '"
check no-position-no-input-form 0   ""            empty                sh -c "'$COMPILER' 2>&1 >/dev/null | head -n 1 | grep -q '^goo: error: '"
check unknown-flag            1     ""            nonempty             "$COMPILER" --totally-bogus-flag "$WORK/ok.goo"
check link-failure            1     ""            "-lgootest_bogus_xyz" "$COMPILER" "$WORK/ok.goo" -l gootest_bogus_xyz -o "$WORK/lf_bin"
check build-extra-arg         1     ""            "unexpected argument" "$COMPILER" build "$WORK/ok.goo" stray_arg
check run-emit-llvm-conflict  1     ""            "cannot be combined" "$COMPILER" run --emit-llvm "$WORK/ok.goo"
check help-on-stdout          0     "~Usage:"     empty                "$COMPILER" help
check snippet-second-file     1     ""            "var second int = \"in second file\"" sh -c "cd '$WORK/twofile' && '$COMPILER' build ."
check snippet-dot-slash-name  1     ""            " | 	var x int = \"not an int\"" sh -c "cd '$WORK' && '$COMPILER' ./dotslash.goo -o dot_bin"
# Directory entry package (`goo build .`). The output is named after the
# DIRECTORY, which is Go's rule for `go build .`, so the binary is dirpkg.
check dir-build                0     ""            empty                sh -c "cd '$WORK/dirpkg' && '$COMPILER' build ."
check dir-binary-runs          0     "hello 42"    empty                "$WORK/dirpkg/dirpkg"
# Building ./dirpkg from the PARENT would name the output after the directory
# that already exists there. Real go1.26.1 refuses with this same wording.
check dir-output-collision     1     ""            "already exists and is a directory" sh -c "cd '$WORK' && '$COMPILER' build ./dirpkg"
check dir-no-sources           1     ""            "no buildable source files" "$COMPILER" build "$WORK/emptydir"
# `goo test` on a package with no _test files: Go prints a no-tests line and
# exits 0 rather than treating it as an error.
check test-no-test-files       0     "~[no test files]" empty  sh -c "cd '$WORK/notests' && '$COMPILER' test ."
# Test discovery: a Test-named function of the wrong shape is rejected by name,
# with the expected signature spelled out. The positive row is the guard that
# the rejection is targeted rather than blanket.
check test-bad-signature       1     ""            "must have signature func(t *testing.T)" sh -c "cd '$WORK/badsig' && '$COMPILER' test ."
check test-good-signature      0     "~ok"         empty                sh -c "cd '$WORK/goodsig' && '$COMPILER' test ."
# Synthesis: `goo test` on a package with NO hand-written main runs its tests.
# The failure row is the one that proves a discovered test was really called.
check test-runs-tests          0     "~ok"         empty                sh -c "cd examples/testpkg && '$COMPILER' test ."
check test-reports-failure     1     "~--- FAIL: TestBoom" empty        sh -c "cd '$WORK/failpkg' && '$COMPILER' test ."
check test-empty-test-file     0     "~ok"         empty                sh -c "cd '$WORK/emptytests' && '$COMPILER' test ."
# A package main PROGRAM must be testable: the synthesized main replaces the
# program's own, which is what `go test` does for package main too.
check test-package-main       0     "~ok"         empty                sh -c "cd '$WORK/mainpkg' && '$COMPILER' test ."
# ...and the program itself still builds and runs normally.
check test-package-main-builds 0    "42"          empty                sh -c "cd '$WORK/mainpkg' && '$COMPILER' run ."

# M2: a source file that is a pipe has no size, so the reader cannot use
# ftell to size its buffer. The file reaches the compiler through a pipe, not
# a regular file. bash sets pipefail so a failing cat cannot hide behind goo.
check stdin-source-builds     0     ""            empty                bash -c "set -o pipefail; cat '$WORK/ok.goo' | timeout 10 '$COMPILER' build -o '$WORK/stdin_bin' /dev/stdin"
check stdin-binary-runs       0     "ok"          empty                "$WORK/stdin_bin"

# L5: a FIFO named like a source file must be skipped. Opening it blocks
# forever, so every run carries timeout 10 and a hang fails the row.
FIFO_DIR=$(mktemp -d "$WORK/fifo.XXXXXX") || { echo "  FAIL fifo-setup: mktemp"; exit 1; }
cp "$WORK/ok.goo" "$FIFO_DIR/main.go"
mkfifo "$FIFO_DIR/y.go" || { echo "  FAIL fifo-setup: mkfifo"; exit 1; }
check fifo-source-skipped     0     ""            empty                sh -c "cd '$FIFO_DIR' && timeout 10 '$COMPILER' build ."
rm -rf "$FIFO_DIR"

# M4: ast_add_child appends in constant time, so the parse of a call with
# 200,000 arguments finishes well under the 5 s deadline (0.5 s measured on
# 2026-11-27). This row checks the parse only. A full `goo build` of the same
# input took 41 s on 2026-11-27: a later stage is still quadratic. The
# follow-up is coord task goolang:A-m4-build-quadratic.
python3 -c '
with open("'"$WORK"'/large_args.goo", "w") as f:
    f.write("package main\n\nfunc f(a ...int) {}\n\nfunc main() {\n    f(")
    f.write(", ".join(["1"] * 200000))
    f.write(")\n}\n")
'
check large-arg-list          0     ""            empty                sh -c "timeout 5 '$COMPILER' --emit-ast-json '$WORK/large_args.goo' >/dev/null"

# A long sibling list must not cost one stack frame per node when the AST is
# freed. 200,000 statements in one block overflowed the 8 MiB stack in
# ast_node_free (SIGSEGV, exit 139) after the dump was written.
python3 -c '
with open("'"$WORK"'/long_block.goo", "w") as f:
    f.write("package main\n\nfunc main() {\n")
    for i in range(100000):
        f.write("    x%d := 1\n    _ = x%d\n" % (i, i))
    f.write("}\n")
'
check long-block-free         0     ""            empty                sh -c "timeout 20 '$COMPILER' --emit-ast-json '$WORK/long_block.goo' >/dev/null"

# A copied or moved compiler has no lib/ beside it. It must find the runtime
# archive through GOOROOT, from a working directory that is not the repo root.
ROOT_ABS="$(pwd)"
mkdir -p "$WORK/moved"
cp "$COMPILER" "$WORK/moved/goo"
check moved-compiler-gooroot  0     ""            empty                sh -c "cd '$WORK/moved' && env -u GOO_RUNTIME GOOROOT='$ROOT_ABS' ./goo '$ROOT_ABS/$WORK/ok.goo' -o moved_bin"

echo "--- cli_test: $pass passed, $fail failed ---"
[ "$fail" -eq 0 ]
