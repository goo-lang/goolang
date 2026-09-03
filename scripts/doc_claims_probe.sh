#!/usr/bin/env bash
# doc-claims-probe — the factual claims our DOCUMENTS make about this codebase,
# as executable assertions.
#
# WHY THIS EXISTS. On 2026-08-20 four claims went into ADR 0006 and its specs
# from inference rather than execution, and three were merged before being
# caught. One said "no file in src/ reads spec->alias"; six do. A prose rule
# telling a future author to check first is only as good as that author's
# discipline on the day. An assertion is not.
#
# THE RULE THIS ENFORCES: a document may state a checkable fact about the tree
# only if something re-checks it. Every claim below is one a reader would act
# on, and every one of them can rot silently.
#
# Assertions are structural on purpose. Behaviour is covered by the golden and
# reject suites; this covers the facts those suites cannot see.
set -u

PROBE="doc-claims-probe"
# A Bazel sh_test starts in the runfiles root, and $0 there is a symlink
# under tests/probes/ -- one level too deep for the old dirname-based ROOT.
# git rev-parse names the real toplevel under make and by hand; it fails in
# the sandbox (no .git), where pwd is already the runfiles root.
ROOT="${DOC_CLAIMS_ROOT:-$(git rev-parse --show-toplevel 2>/dev/null || pwd)}"
fails=0
note() { echo "    $*"; }

# --- --self-test: every assertion above must be shown capable of going RED.
# A claim-checker that cannot fail is worth exactly as much as the prose it
# replaced. Four injections, plus a CONTROL run on the unmutated copy -- without
# the control, a broken temp tree would turn everything red and look like
# success.
if [ "${1:-}" = "--self-test" ]; then
	SELF="$PROBE --self-test"
	mk_tree() {
		local t="$1"
		mkdir -p "$t/scripts" "$t/src/compiler" "$t/src/codegen" "$t/src/package"
		cp -r "$ROOT/goostd" "$t/goostd"
		cp "$ROOT/scripts/check_stdlib_coverage.sh" "$t/scripts/"
		cp "$ROOT/src/compiler/goo.c" "$t/src/compiler/"
		cp "$ROOT/src/codegen/codegen.c" "$t/src/codegen/"
		cp "$ROOT/src/package/import_resolver.c" "$t/src/package/"
		cp "$ROOT/Makefile" "$t/"
	}
	run_on() { DOC_CLAIMS_ROOT="$1" "$0" >"$2" 2>&1; }

	W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
	bad_self=0

	# CONTROL: an unmutated copy must be GREEN, or every red below is meaningless.
	mk_tree "$W/ctl"
	if run_on "$W/ctl" "$W/ctl.log"; then
		echo "    ok: control (unmutated copy) is GREEN"
	else
		echo "$SELF: FAIL (the control copy is already red -- the harness is broken, not the tree)"
		sed 's/^/        /' "$W/ctl.log"
		exit 1
	fi

	inject() {  # name, mutation-command
		local name="$1"; shift
		rm -rf "$W/m"; mk_tree "$W/m"
		( cd "$W/m" && eval "$@" ) >/dev/null 2>&1
		if run_on "$W/m" "$W/m.log"; then
			echo "$SELF: FAIL (stayed green after: $name)"
			bad_self=1
		else
			echo "    ok: '$name' turns the probe red"
		fi
	}

	inject "an unclassified goostd package" \
		'mkdir -p goostd/newpkg && printf "package newpkg\nfunc F() int { return 1 }\n" > goostd/newpkg/newpkg.go'
	inject "the shim list changes" \
		'sed -i "s/\"testing\"};/\"testing\", \"json\"};/" src/compiler/goo.c'
	inject "a third /proc/self/exe site" \
		'printf "// /proc/self/exe\n" >> src/package/import_resolver.c && cp src/package/import_resolver.c src/compiler/third.c'
	inject "make install regresses to a bare cp" \
		'python3 - <<PY
import io,re
s=io.open("Makefile").read()
s=re.sub(r"^install:.*?\n\n", "install: $(COMPILER)\n\tcp $(COMPILER) /usr/local/bin/\n\n", s, count=1, flags=re.S|re.M)
io.open("Makefile","w").write(s)
PY'

	[ "$bad_self" -ne 0 ] && exit 1
	echo "$SELF: PASS (control green; all 4 claims independently turn it red)"
	exit 0
fi
bad()  { echo "  FAIL: $*"; fails=$((fails+1)); }

# --- 1. Every goostd package is classified: stdlib, or a declared fixture.
# goostd/ holds 15 directories and only 9 ship. package_toolchain.sh reads
# GOOSTD_PKG_DIRS, so a NEW package that nobody adds there is silently absent
# from every release -- no error, no warning, just missing. The other six say
# "Fixture" in their own header. A package that is neither is unclassified,
# and unclassified means nobody has decided whether users get it.
PKGS="$(sed -n 's/^GOOSTD_PKG_DIRS="\(.*\)"$/\1/p' "$ROOT/scripts/check_stdlib_coverage.sh")"
if [ -z "$PKGS" ]; then
	bad "could not read GOOSTD_PKG_DIRS (the stdlib list) from check_stdlib_coverage.sh"
else
	shipped=0; fixture=0; unclassified=0
	for d in "$ROOT"/goostd/*/; do
		name="$(basename "$d")"
		if printf '%s' "$PKGS" | tr ' ' '\n' | grep -q "^${name}:"; then
			shipped=$((shipped+1)); continue
		fi
		# -R, not -r: under Bazel "$d" is a real directory of SYMLINKED
		# files (a sh_test's data arrives as a runfiles tree), and plain
		# -r does not follow a symlink found while recursing, so every
		# package read as unclassified until this was measured and fixed.
		if grep -Rqil 'fixture' "$d" --include='*.go' --include='*.goo' 2>/dev/null; then
			fixture=$((fixture+1)); continue
		fi
		bad "goostd/$name is neither in GOOSTD_PKG_DIRS nor self-declared a fixture"
		note "it would be silently absent from every released toolchain"
		unclassified=$((unclassified+1))
	done
	[ "$unclassified" -eq 0 ] && note "goostd: $shipped shipped, $fixture fixtures, 0 unclassified"
fi

# --- 2. The shim package set, which ADR 0006 states as eight names. These
# short-circuit before the resolver, so a fetched package can never shadow
# them -- a fact sub-project B's design depends on.
EXPECTED_SHIMS='"fmt", "os", "math", "errors", "sync", "time", "far", "testing"'
# EXACT comparison, not `grep -qF`. The first version of this check used a
# substring match, and its own self-test caught the hole: adding a ninth shim
# leaves the eight-name string present as a substring, so an ADDITION -- the
# likeliest change of all -- passed silently.
ACTUAL_SHIMS="$(sed -n 's/.*static const char\* const shim\[\] = {\(.*\)};.*/\1/p' "$ROOT/src/compiler/goo.c" | head -1)"
if [ "$ACTUAL_SHIMS" != "$EXPECTED_SHIMS" ]; then
	bad "the shim list in goo.c no longer matches the eight names ADR 0006 states"
	note "expected: $EXPECTED_SHIMS"
	note "found   : ${ACTUAL_SHIMS:-<no shim[] initialiser found>}"
else
	note "shim set: the documented eight, exactly"
fi

# --- 3. /proc/self/exe lives in exactly two files. ADR 0006 names extracting
# one goo_exe_dir() helper as the PREREQUISITE for any non-Linux platform,
# because the platform ladder would otherwise be written twice. A third copy
# makes that prerequisite quietly bigger than the ADR says.
# -R, same reason as the fixture scan above: $ROOT/src is a directory of
# symlinks under Bazel, and -r does not follow one found while recursing.
sites=$(grep -Rl '/proc/self/exe' "$ROOT/src" --include='*.c' 2>/dev/null | wc -l)
if [ "$sites" -ne 2 ]; then
	bad "/proc/self/exe appears in $sites files; ADR 0006 documents 2"
	note "$(grep -Rl '/proc/self/exe' "$ROOT/src" --include='*.c' 2>/dev/null | tr '\n' ' ')"
else
	note "/proc/self/exe: 2 sites, as documented"
fi

# --- 4. `make install` goes through the shared packaging path. It was once
# `cp $(COMPILER) /usr/local/bin/` and nothing else, which produced an
# installation that only worked from the repository root (ADR 0006 defect 1).
# A regression to a bare cp would pass every other gate in the tree.
if ! sed -n '/^install:/,/^$/p' "$ROOT/Makefile" | grep -q 'package_toolchain.sh'; then
	bad "make install no longer goes through package_toolchain.sh"
	note "an install that copies only bin/goo cannot resolve goostd (ADR 0006 defect 1)"
else
	note "make install: uses the shared packaging path"
fi

if [ "$fails" -ne 0 ]; then
	echo "$PROBE: FAIL ($fails claim(s) no longer hold)"
	exit 1
fi
echo "$PROBE: PASS (4 documented claims re-checked against the tree)"
exit 0
