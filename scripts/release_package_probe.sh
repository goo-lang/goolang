#!/usr/bin/env bash
# release-package-probe — an EXTRACTED toolchain tarball must compile and run a
# program with no environment variable set and from an unrelated directory.
#
# THIS IS THE FIRST AUTOMATED EXECUTION OF resolver tier 2. `goo_gooroot_dir()`
# (src/package/import_resolver.c:49) and `goo_runtime_archive_path()`
# (src/codegen/codegen.c) both look at `<exe-dir>/../lib/...`, and nothing in
# the tree has ever built that layout: the dev tree keeps goostd/ at the root,
# so it resolves through tier 3. The whole distribution design in ADR 0006
# rests on tier 2 working.
#
# THREE THINGS THIS PROBE MUST NOT GET WRONG, each measured rather than assumed:
#
#   1. The test program MUST import a vendored goostd package. A `fmt`-only
#      hello-world compiles and runs with lib/goostd DELETED, because the eight
#      shim packages (is_stdlib_shim_import, src/compiler/goo.c) are inside
#      bin/goo and never call goo_gooroot_dir(). The obvious probe passes
#      forever while testing nothing.
#   2. It MUST run from an unrelated cwd. From the repo root the cwd fallback
#      (tier 4) answers, and tier 2 is never reached.
#   3. It MUST unset BOTH GOOROOT and GOO_RUNTIME. Unsetting one leaves the
#      other lookup untested.
#
# Exit statuses are taken with no pipe, per the convention the other probes
# use: a piped status reports the last stage, not the program under test.
set -u

PROBE="release-package-probe"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SELFTEST=0
[ "${1:-}" = "--self-test" ] && SELFTEST=1

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Build the tarball through the real packaging path, never by hand here.
if ! "$ROOT/scripts/package_toolchain.sh" "$WORK/dist" >"$WORK/pack.log" 2>&1; then
	echo "$PROBE: FAIL (packaging failed)"
	sed 's/^/    /' "$WORK/pack.log"
	exit 1
fi
TARBALL="$(find "$WORK/dist" -name 'goo-*.tar.gz' | head -1)"
if [ -z "$TARBALL" ]; then
	echo "$PROBE: FAIL (packaging produced no tarball)"
	exit 1
fi

extract() {
	rm -rf "$WORK/root"; mkdir -p "$WORK/root"
	tar -xzf "$TARBALL" -C "$WORK/root" --strip-components=1
}

# The fixture imports `strings`, a VENDORED package, for reason 1 above.
mkdir -p "$WORK/away"
cat > "$WORK/away/probe.goo" <<'GOO'
package main

import (
	"fmt"
	"strings"
)

func main() {
	fmt.Println(strings.ToUpper("tier two"))
}
GOO

# Run the extracted toolchain: unrelated cwd, both variables unset.
run_extracted() {
	( cd "$WORK/away" && env -u GOOROOT -u GOO_RUNTIME \
	    "$WORK/root/bin/goo" run probe.goo ) >"$WORK/run.log" 2>&1
	return $?
}

if [ "$SELFTEST" -eq 0 ]; then
	extract
	run_extracted
	rc=$?
	if [ $rc -ne 0 ]; then
		echo "$PROBE: FAIL (extracted toolchain could not build the fixture; exit=$rc)"
		sed 's/^/    /' "$WORK/run.log"
		exit 1
	fi
	if ! grep -q '^TIER TWO$' "$WORK/run.log"; then
		echo "$PROBE: FAIL (ran, but produced the wrong output)"
		sed 's/^/    /' "$WORK/run.log"
		exit 1
	fi
	# Positive evidence that the fixtures are NOT shipped as stdlib.
	for fixture in cpkg fwdref pkgcheck kinds shapes mypkg; do
		if [ -d "$WORK/root/lib/goostd/$fixture" ]; then
			echo "$PROBE: FAIL (compiler test fixture '$fixture' shipped as stdlib)"
			exit 1
		fi
	done
	# Packaging determinism. ADR 0006 named tar/gzip as a NEW input to a
	# reproducibility claim and left it unproven; this proves it. gzip stamps
	# an mtime without -n, and tar records member order, mtime, uid and gid --
	# the same class of defect `ar` had before `ar -D`. Measured here rather
	# than trusted to the flags being correct.
	if ! "$ROOT/scripts/package_toolchain.sh" "$WORK/dist2" >>"$WORK/pack.log" 2>&1; then
		echo "$PROBE: FAIL (second packaging run failed)"
		exit 1
	fi
	h1="$(cat "$WORK"/dist/*.sha256)"
	h2="$(cat "$WORK"/dist2/*.sha256)"
	if [ "$h1" != "$h2" ]; then
		echo "$PROBE: FAIL (packaging is not deterministic)"
		echo "    run 1: $h1"
		echo "    run 2: $h2"
		exit 1
	fi

	echo "$PROBE: PASS (tier-2 layout resolves goostd and the runtime archive, no env, unrelated cwd; packaging deterministic)"
	exit 0
fi

# --- self-test: TWO injections, because one lookup passing does not prove the
# --- other one runs at all. Each asserts the removal landed before trusting red.
fail=0
for victim in lib/goostd lib/libgoo_runtime.a; do
	extract
	rm -rf "${WORK:?}/root/$victim"
	if [ -e "$WORK/root/$victim" ]; then
		echo "$PROBE --self-test: FAIL (injection did not land: $victim still present)"
		exit 1
	fi
	run_extracted
	rc=$?
	if [ $rc -eq 0 ]; then
		echo "$PROBE --self-test: FAIL (probe stayed green with $victim removed)"
		sed 's/^/    /' "$WORK/run.log"
		fail=1
	else
		echo "    ok: removing $victim turns the probe red (exit=$rc)"
	fi
done
[ $fail -ne 0 ] && exit 1
echo "$PROBE --self-test: PASS (both tier-2 lookups are load-bearing)"
exit 0
