#!/usr/bin/env bash
# Gate for the OpenQASM 3 and QIR text that bin/goo writes.
#
# Before this gate no probe in verify-core read --emit-qasm3 or --emit-qir
# output, so the emitter could write a wrong circuit and exit 0. The fixtures
# in tests/quantum_emit hold two contracts:
#
#   NAME.qasm     The source is QE_DIR/NAME.goo if it exists, else
#                 examples/NAME.goo. `goo --emit-qasm3 SRC -o OUT` must exit 0
#                 and OUT must equal NAME.qasm byte for byte. The same source
#                 must also give a QIR file (--emit-qir) that holds the
#                 adaptive_profile and entry_point attributes. QIR text depends
#                 on the LLVM version, so it is a smoke check, not a diff.
#   NAME.err.txt  The source is QE_DIR/NAME.goo. `--emit-qasm3` must exit
#                 non-zero, must not create OUT, and its stderr must contain
#                 the text of NAME.err.txt (first line).
#
# A gate that finds zero fixtures is broken, not green. Exit codes are read
# directly, never through a pipe.
# Usage: quantum_emit_probe.sh [--self-test]
# Environment: QE_ROOT, GOO_BIN, QE_DIR.
set -u
QE_ROOT="${QE_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
GOO_BIN="${GOO_BIN:-$QE_ROOT/bin/goo}"
QE_DIR="${QE_DIR:-$QE_ROOT/tests/quantum_emit}"
SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
TIMEOUT_S=20

run_check() {
	local tmp fails=0 nq=0 nr=0 nk=0 f name src rc
	tmp=$(mktemp -d) || { echo "quantum-emit-probe: FAIL (no temp dir)"; return 1; }
	trap 'rm -rf "$tmp"' RETURN
	fail() { echo "quantum-emit-probe: FAIL ($1)"; fails=$((fails + 1)); }

	for f in "$QE_DIR"/*.qasm; do
		[ -e "$f" ] || break
		name=$(basename "$f" .qasm)
		src="$QE_DIR/$name.goo"
		[ -f "$src" ] || src="$QE_ROOT/examples/$name.goo"
		nq=$((nq + 1))
		if [ ! -f "$src" ]; then
			fail "$name: no source"
			continue
		fi
		rm -f "$tmp/out.qasm"
		timeout "$TIMEOUT_S" "$GOO_BIN" --emit-qasm3 "$src" -o "$tmp/out.qasm" > "$tmp/log" 2>&1
		rc=$?
		if [ "$rc" -ne 0 ]; then
			fail "$name: qasm compile exit $rc"
		elif [ ! -s "$tmp/out.qasm" ]; then
			fail "$name: no output file"
		elif ! cmp -s "$tmp/out.qasm" "$f"; then
			fail "$name: qasm differs"
		fi
		rm -f "$tmp/out.ll"
		timeout "$TIMEOUT_S" "$GOO_BIN" --emit-qir "$src" -o "$tmp/out.ll" > "$tmp/log" 2>&1
		rc=$?
		if [ "$rc" -ne 0 ]; then
			fail "$name: qir compile exit $rc"
		elif [ ! -s "$tmp/out.ll" ]; then
			fail "$name: qir no output file"
		elif ! grep -q '"qir_profiles"="adaptive_profile"' "$tmp/out.ll" ||
			! grep -q '"entry_point"' "$tmp/out.ll"; then
			fail "$name: qir profile attribute missing"
		else
			nk=$((nk + 1))
		fi
	done

	for f in "$QE_DIR"/*.err.txt; do
		[ -e "$f" ] || break
		name=$(basename "$f" .err.txt)
		src="$QE_DIR/$name.goo"
		nr=$((nr + 1))
		if [ ! -f "$src" ]; then
			fail "$name: no source"
			continue
		fi
		rm -f "$tmp/out.qasm"
		timeout "$TIMEOUT_S" "$GOO_BIN" --emit-qasm3 "$src" -o "$tmp/out.qasm" > /dev/null 2> "$tmp/err"
		rc=$?
		if [ "$rc" -eq 0 ]; then
			fail "$name: compiled, expected a refusal"
		elif [ "$rc" -eq 124 ]; then
			fail "$name: timeout"
		elif [ -e "$tmp/out.qasm" ]; then
			fail "$name: output file written by a refusal"
		elif ! grep -qF -- "$(head -n 1 "$f")" "$tmp/err"; then
			fail "$name: stderr lacks the expected text"
		fi
	done

	if [ $((nq + nr)) -eq 0 ]; then
		fail "no fixtures"
	fi
	if [ "$fails" -ne 0 ]; then
		return 1
	fi
	echo "quantum-emit-probe: PASS ($nq qasm, $nr refusals, $nk qir)"
}

# expect_fail LABEL NEEDLE: the last probe run (in $OUT, $RC) must fail and its
# output must hold NEEDLE, so each mutant is refused for its own reason.
st_fail=0
expect_pass() {
	if [ "$RC" -ne 0 ] || ! grep -qF "quantum-emit-probe: PASS" <<<"$OUT"; then
		echo "self-test FAIL: $1 was not accepted: $OUT"
		st_fail=$((st_fail + 1))
	fi
}
expect_fail() {
	if [ "$RC" -eq 0 ] || ! grep -qF -- "$2" <<<"$OUT"; then
		echo "self-test FAIL: $1 was not refused with '$2': rc=$RC $OUT"
		st_fail=$((st_fail + 1))
	fi
}

# probe_with QE_DIR GOO: runs the check mode in a child so its trap and
# variables stay out of the self-test.
probe_with() {
	OUT=$(QE_ROOT="$QE_ROOT" QE_DIR="$1" GOO_BIN="$2" bash "$SELF" 2>&1)
	RC=$?
}

self_test() {
	local t d ref=qir_teleport_probe real_dir stub
	real_dir="$QE_ROOT/tests/quantum_emit"
	[ -f "$real_dir/$ref.qasm" ] || { echo "self-test FAIL: no $ref.qasm control in $real_dir"; return 1; }
	[ -x "$GOO_BIN" ] || { echo "self-test FAIL: $GOO_BIN is not executable"; return 1; }
	t=$(mktemp -d) || return 1
	trap 'rm -rf "$t"' RETURN

	# control: the real fixture and the real compiler pass.
	d="$t/ok"; mkdir "$d"; cp "$real_dir/$ref.qasm" "$d/"
	probe_with "$d" "$GOO_BIN"
	expect_pass "control"

	# a changed byte in the reference.
	d="$t/bytes"; mkdir "$d"; cp "$real_dir/$ref.qasm" "$d/"
	sed -i '0,/OPENQASM 3.0;/s//OPENQASM 3.1;/' "$d/$ref.qasm"
	if cmp -s "$d/$ref.qasm" "$real_dir/$ref.qasm"; then
		echo "self-test FAIL: the byte mutation did not land"; st_fail=$((st_fail + 1))
	fi
	probe_with "$d" "$GOO_BIN"
	expect_fail "a changed qasm byte" "FAIL ($ref: qasm differs)"

	# a compiler that exits 0 and writes nothing.
	stub="$t/stub_empty"
	printf '#!/bin/sh\nexit 0\n' > "$stub"; chmod +x "$stub"
	probe_with "$t/ok" "$stub"
	expect_fail "a stub that writes no file" "FAIL ($ref: no output file)"

	# a compiler whose QIR output lacks the profile attribute.
	stub="$t/stub_noprofile"
	cat > "$stub" <<STUB
#!/bin/sh
# --emit-qasm3 goes to the real compiler. --emit-qir writes a bare file.
case "\$1" in
--emit-qir) shift; while [ \$# -gt 0 ]; do [ "\$1" = -o ] && echo 'define void @main()' > "\$2"; shift; done; exit 0 ;;
*) exec "$GOO_BIN" "\$@" ;;
esac
STUB
	chmod +x "$stub"
	probe_with "$t/ok" "$stub"
	expect_fail "a QIR file without the profile attribute" "FAIL ($ref: qir profile attribute missing)"

	# a refusal fixture whose program compiles.
	d="$t/compiles"; mkdir "$d"; cp "$real_dir/$ref.qasm" "$d/"
	cp "$QE_ROOT/examples/$ref.goo" "$d/okprog.goo"
	echo "some refusal text" > "$d/okprog.err.txt"
	probe_with "$d" "$GOO_BIN"
	expect_fail "a refusal fixture that compiles" "FAIL (okprog: compiled, expected a refusal)"

	# the refusal path with a stub: right text passes, wrong text and a
	# written output file are each refused.
	d="$t/refuse"; mkdir "$d"
	echo "int Foo()" > "$d/rj.goo"; echo "cannot resolve widget" > "$d/rj.err.txt"
	stub="$t/stub_refuse"
	printf '#!/bin/sh\necho "rj.goo:1:1: error: cannot resolve widget" >&2\nexit 1\n' > "$stub"; chmod +x "$stub"
	probe_with "$d" "$stub"
	expect_pass "a refusal with the right text"
	stub="$t/stub_wrongtext"
	printf '#!/bin/sh\necho "other" >&2\nexit 1\n' > "$stub"; chmod +x "$stub"
	probe_with "$d" "$stub"
	expect_fail "a refusal with the wrong text" "FAIL (rj: stderr lacks the expected text)"
	stub="$t/stub_writes"
	cat > "$stub" <<'STUB'
#!/bin/sh
echo "cannot resolve widget" >&2
while [ $# -gt 0 ]; do [ "$1" = -o ] && echo partial > "$2"; shift; done
exit 1
STUB
	chmod +x "$stub"
	probe_with "$d" "$stub"
	expect_fail "a refusal that writes the output file" "FAIL (rj: output file written by a refusal)"

	# an empty fixture directory.
	d="$t/empty"; mkdir "$d"
	probe_with "$d" "$GOO_BIN"
	expect_fail "an empty fixture directory" "FAIL (no fixtures)"

	if [ "$st_fail" -ne 0 ]; then
		echo "quantum-emit-selftest: FAIL ($st_fail)"
		return 1
	fi
	echo "quantum-emit-selftest: PASS"
}

case "${1:-}" in
--self-test) self_test ;;
"") run_check ;;
*) echo "usage: $0 [--self-test]" >&2; exit 2 ;;
esac
