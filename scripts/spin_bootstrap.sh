#!/usr/bin/env bash
# spin-bootstrap -- produce a usable `spin` binary (the SPIN model checker,
# Track B's Promela gate, tasks 5-7) at version 6.5 or newer, with no network
# read. Fedora has no SPIN package, so a pinned source tarball is vendored
# (third_party/spin-6.5.2.tar.gz, see third_party/README.md) and this script
# builds it the same way third_party/nng-1.12.0.tar.gz is built by the
# Makefile -- except SPIN needs no library-caller wiring, so a standalone
# script (not a Makefile recipe) is enough.
#
# Order of preference, cheapest first:
#   1. `spin` on PATH, if `spin -V` reports 6.5 or newer.
#   2. $SPIN_BUILD_DIR/spin/bin/spin (a prior run of this script), if it
#      reports 6.5 or newer.
#   3. Build it: verify third_party/spin-6.5.2.tar.gz's sha256 against the
#      value hard-coded below (a mismatch is exit 3, never a silent build of
#      the wrong bytes); extract into $SPIN_BUILD_DIR/spin-src with
#      --strip-components=1; `make` in Src/ with the makefile's own default
#      flags plus -Wno-error=implicit-function-declaration (spin.y:1099
#      calls `free` with no <stdlib.h>, and gcc 14+ makes that an error by
#      default -- see third_party/README.md for the measurement); copy
#      Src/spin to $SPIN_BUILD_DIR/spin/bin/spin.
#
# Prints the absolute path of the usable spin binary as its LAST stdout
# line and exits 0 on success. On any failure it prints a message on
# stderr and exits non-zero (3 for a sha256 mismatch, 1 for anything else,
# 2 for a usage error).
#
# SPIN_TARBALL and SPIN_BUILD_DIR override the tarball path and the build
# root -- the self-test points both at scratch copies so it never touches
# the real third_party/ tarball or build/tools/ tree.
#
# SPIN_BOOTSTRAP_IGNORE_PATH="1" skips step 1 (a `spin` already on PATH)
# outright, forcing SPIN_BUILD_DIR / build-from-tarball instead.
#
# INCIDENT, 2026-09-03: a `spin` binary appeared at
# /home/ddowney/.local/bin/spin partway through an unrelated task's
# `make verify-core` run -- nothing in this script, or anything that calls
# it, ever installs there, so this was another process on the same
# machine, outside this repository's control. The self-test's negative
# control (a corrupted tarball must exit 3) went red for the WRONG
# reason: the PATH spin answered "usable" before build_spin ever looked at
# the tarball, so the corrupted bytes were never consulted. Its
# build-then-reuse pair went red the same way, one step later: the
# "positive control" step also took the PATH shortcut instead of building
# into the scratch dir, so the following "second run must reuse, not
# rebuild" step found nothing there to reuse and built for real -- read as
# an unwanted rebuild. Three controls had never once been exercised with a
# real `spin` on PATH. SPIN_BOOTSTRAP_IGNORE_PATH is what the self-test
# sets on all three (they share one build directory, so the negative
# control, the build, and the reuse check must be scrubbed as one unit,
# not the reuse check alone) -- see self_test() below. Control 4 (an OLD
# PATH spin must be rejected) is deliberately NOT scrubbed: it is what
# still exercises the PATH-preference logic itself, against a fake spin on
# a private PATH it controls.
#
# Usage:
#   scripts/spin_bootstrap.sh              # print a usable spin's path
#   scripts/spin_bootstrap.sh --self-test  # prove it can go red and green
set -u

PROBE="spin-bootstrap"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

SPIN_VERSION_PINNED="6.5.2"
SPIN_VERSION_MIN="6.5"
SPIN_SHA256="e46a3bd308c4cd213cc466a8aaecfd5cedc02241190f3cb9a1d1b87e5f37080a"
SPIN_BOOTSTRAP_IGNORE_PATH="${SPIN_BOOTSTRAP_IGNORE_PATH:-0}"
SPIN_MAKE_CFLAGS="-O2 -DNXT -Wall -pedantic -Wno-error=implicit-function-declaration"

SPIN_TARBALL="${SPIN_TARBALL:-$REPO_ROOT/third_party/spin-$SPIN_VERSION_PINNED.tar.gz}"
SPIN_BUILD_DIR="${SPIN_BUILD_DIR:-$REPO_ROOT/build/tools}"

SPIN_SRC_DIR="$SPIN_BUILD_DIR/spin-src"
SPIN_BIN="$SPIN_BUILD_DIR/spin/bin/spin"

# spin_version: print "MAJOR.MINOR" parsed from "<candidate> -V", or print
# nothing and return 1 on any failure -- missing file, not executable, a
# nonzero exit, or output that does not match SPIN's own "Spin Version
# X.Y.Z -- <date>" banner. Never trusts an empty read as a version.
spin_version() { # $1 = path to a candidate spin binary
	local candidate="$1" out
	[ -x "$candidate" ] || return 1
	out="$("$candidate" -V 2>&1)" || return 1
	printf '%s\n' "$out" | grep -oE 'Version [0-9]+\.[0-9]+' | head -n1 | grep -oE '[0-9]+\.[0-9]+'
}

# version_ge: true when "$1" (MAJOR.MINOR) is >= "$2" (MAJOR.MINOR). Compares
# the two fields as integers, not as strings, so "6.10" is correctly newer
# than "6.9".
version_ge() {
	local a_major="${1%%.*}" a_minor="${1#*.}" b_major="${2%%.*}" b_minor="${2#*.}"
	if [ "$a_major" -gt "$b_major" ] 2>/dev/null; then return 0; fi
	if [ "$a_major" -eq "$b_major" ] 2>/dev/null && [ "$a_minor" -ge "$b_minor" ] 2>/dev/null; then return 0; fi
	return 1
}

# usable_spin: print the absolute path of $1 on stdout, and return 0, only
# when it exists, is executable, and its own "-V" reports SPIN_VERSION_MIN
# or newer. This is the ONE gate for both the PATH candidate and a
# previously built one -- an old PATH spin is rejected here exactly the
# same way a stale build/tools/spin would be, never accepted just because
# it exists.
usable_spin() { # $1 = path to a candidate spin binary
	local candidate="$1" ver
	ver="$(spin_version "$candidate")" || return 1
	[ -n "$ver" ] || return 1
	version_ge "$ver" "$SPIN_VERSION_MIN" || return 1
	printf '%s\n' "$candidate"
}

# build_spin: verify the tarball, extract, build, and install to $SPIN_BIN.
# Returns 3 for a sha256 mismatch (the one case the task's accept criterion
# names a specific exit code for), 1 for any other build failure. Prints a
# message on stderr either way; never silent.
build_spin() {
	if [ ! -f "$SPIN_TARBALL" ]; then
		echo "$PROBE: FAIL ($SPIN_TARBALL: no such file -- cannot verify its sha256 or build it)" >&2
		return 1
	fi
	local actual
	actual="$(sha256sum "$SPIN_TARBALL" | cut -d' ' -f1)"
	if [ "$actual" != "$SPIN_SHA256" ]; then
		echo "$PROBE: FAIL ($SPIN_TARBALL sha256 mismatch -- expected $SPIN_SHA256, got $actual)" >&2
		return 3
	fi
	rm -rf "$SPIN_SRC_DIR"
	mkdir -p "$SPIN_SRC_DIR"
	if ! tar -xzf "$SPIN_TARBALL" -C "$SPIN_SRC_DIR" --strip-components=1; then
		echo "$PROBE: FAIL (tar could not extract $SPIN_TARBALL into $SPIN_SRC_DIR)" >&2
		return 1
	fi
	if [ ! -f "$SPIN_SRC_DIR/Src/makefile" ]; then
		echo "$PROBE: FAIL ($SPIN_SRC_DIR/Src/makefile missing after extraction -- unexpected tarball layout)" >&2
		return 1
	fi
	local build_log="$SPIN_BUILD_DIR/spin-build.log"
	mkdir -p "$SPIN_BUILD_DIR"
	if ! ( cd "$SPIN_SRC_DIR/Src" && make CFLAGS="$SPIN_MAKE_CFLAGS" -j"$(nproc)" ) >"$build_log" 2>&1; then
		echo "$PROBE: FAIL (make failed in $SPIN_SRC_DIR/Src -- see $build_log)" >&2
		return 1
	fi
	if [ ! -x "$SPIN_SRC_DIR/Src/spin" ]; then
		echo "$PROBE: FAIL (make in $SPIN_SRC_DIR/Src did not produce an executable spin -- see $build_log)" >&2
		return 1
	fi
	mkdir -p "$(dirname "$SPIN_BIN")"
	cp "$SPIN_SRC_DIR/Src/spin" "$SPIN_BIN"
	local ver
	ver="$(spin_version "$SPIN_BIN")"
	if [ -z "$ver" ] || ! version_ge "$ver" "$SPIN_VERSION_MIN"; then
		echo "$PROBE: FAIL (built $SPIN_BIN but 'spin -V' did not report $SPIN_VERSION_MIN or newer)" >&2
		return 1
	fi
	echo "$PROBE: built $SPIN_BIN (spin -V reports $ver)" >&2
	return 0
}

# ---------------------------------------------------------------------------
# --self-test: prove the gate can go both red and green.
#
#   1. negative control: a copy of the real tarball with one byte appended
#      (confirmed different from the original with cmp, so the injection is
#      proven to have landed before it is trusted) must exit 3 and name the
#      tarball in its message.
#   2. positive control: the real tarball, against a scratch build dir,
#      must build and 'spin -V' on the printed path must report the pinned
#      version.
#   3. a second run against the SAME scratch build dir must not rebuild --
#      shown by the installed binary's mtime staying put and by the
#      "previously built" skip message.
#   4. extra, beyond the task's three required checks: a PATH `spin` that
#      reports a version below the floor (6.4.9) must be REJECTED, not
#      accepted just because something answered to the name -- the
#      bootstrap must fall through and build its own instead.
# ---------------------------------------------------------------------------
self_test() {
	local SELF="$PROBE --self-test"
	local W bad_self=0
	W="$(mktemp -d)"
	trap 'rm -rf "$W"' RETURN

	local real_tarball="$REPO_ROOT/third_party/spin-$SPIN_VERSION_PINNED.tar.gz"
	if [ ! -f "$real_tarball" ]; then
		echo "$SELF: FAIL ($real_tarball missing -- nothing to self-test against)"
		return 1
	fi

	# 1. Negative control: corrupted tarball -> sha256 mismatch -> exit 3.
	cp "$real_tarball" "$W/bad.tar.gz"
	printf 'x' >>"$W/bad.tar.gz"
	if cmp -s "$real_tarball" "$W/bad.tar.gz"; then
		echo "$SELF: FAIL (appended byte did not change the copy -- cmp reports no difference)"
		return 1
	fi
	echo "    ok: the appended byte landed (cmp confirms the copy differs from $real_tarball)"

	SPIN_TARBALL="$W/bad.tar.gz" SPIN_BUILD_DIR="$W/build-neg" SPIN_BOOTSTRAP_IGNORE_PATH=1 "$0" >"$W/neg.out" 2>"$W/neg.err"
	local rc=$?
	if [ "$rc" -ne 3 ]; then
		echo "$SELF: FAIL (a corrupted tarball exited $rc, want 3)"
		sed 's/^/        /' "$W/neg.err"
		bad_self=1
	elif ! grep -qF "$W/bad.tar.gz" "$W/neg.err"; then
		echo "$SELF: FAIL (exit 3, but the message does not name $W/bad.tar.gz)"
		sed 's/^/        /' "$W/neg.err"
		bad_self=1
	else
		echo "    ok: a corrupted tarball is exit 3, and the message names the file"
	fi

	# 2. Positive control: real tarball, scratch build dir -> build + spin -V.
	# SPIN_BOOTSTRAP_IGNORE_PATH=1 here too, and not only on control 3: this
	# step and control 3 share $pos_build, so if THIS step took a PATH
	# shortcut instead of really building, $pos_build would hold nothing for
	# control 3 to prove it reused -- see the incident note above.
	local pos_build="$W/build-pos"
	local out rc2
	out="$(SPIN_TARBALL="$real_tarball" SPIN_BUILD_DIR="$pos_build" SPIN_BOOTSTRAP_IGNORE_PATH=1 "$0" 2>"$W/pos.err")"
	rc2=$?
	if [ "$rc2" -ne 0 ]; then
		echo "$SELF: FAIL (build from the real tarball exited $rc2)"
		sed 's/^/        /' "$W/pos.err"
		bad_self=1
	else
		local binpath
		binpath="$(printf '%s\n' "$out" | tail -n1)"
		if [ ! -x "$binpath" ]; then
			echo "$SELF: FAIL (printed path [$binpath] is not an executable file)"
			bad_self=1
		else
			local ver_out
			ver_out="$("$binpath" -V 2>&1)"
			if ! printf '%s\n' "$ver_out" | grep -q "Version $SPIN_VERSION_PINNED"; then
				echo "$SELF: FAIL (built spin -V printed [$ver_out], want Version $SPIN_VERSION_PINNED)"
				bad_self=1
			else
				echo "    ok: built from the real tarball against a scratch dir, spin -V reports $SPIN_VERSION_PINNED"
			fi
		fi
	fi

	# 3. Second run, same build dir -> no rebuild (mtime unchanged, and the
	#    skip message is printed).
	local marker="$pos_build/spin/bin/spin"
	local mtime_before mtime_after out2 rc3
	mtime_before="$(stat -c %Y "$marker" 2>/dev/null || echo "")"
	out2="$(SPIN_TARBALL="$real_tarball" SPIN_BUILD_DIR="$pos_build" SPIN_BOOTSTRAP_IGNORE_PATH=1 "$0" 2>"$W/pos2.err")"
	rc3=$?
	mtime_after="$(stat -c %Y "$marker" 2>/dev/null || echo "")"
	if [ "$rc3" -ne 0 ]; then
		echo "$SELF: FAIL (second run against the same build dir exited $rc3)"
		sed 's/^/        /' "$W/pos2.err"
		bad_self=1
	elif [ -z "$mtime_before" ] || [ "$mtime_before" != "$mtime_after" ]; then
		echo "$SELF: FAIL (spin binary mtime changed -- it rebuilt instead of reusing $marker)"
		bad_self=1
	elif ! grep -q "previously built" "$W/pos2.err"; then
		echo "$SELF: FAIL (second run did not print the 'previously built' skip message)"
		sed 's/^/        /' "$W/pos2.err"
		bad_self=1
	else
		echo "    ok: a second run against the same build dir reused $marker with no rebuild"
	fi

	# 4. Extra: an old PATH spin must be rejected, not accepted on name alone.
	local fake_dir="$W/fakepath"
	mkdir -p "$fake_dir"
	cat >"$fake_dir/spin" <<'FAKE_SPIN'
#!/usr/bin/env bash
echo "Spin Version 6.4.9 -- 1 January 2019"
FAKE_SPIN
	chmod +x "$fake_dir/spin"
	local old_path_build="$W/build-old-path"
	local out3 rc4 accepted
	out3="$(PATH="$fake_dir:$PATH" SPIN_TARBALL="$real_tarball" SPIN_BUILD_DIR="$old_path_build" "$0" 2>"$W/old.err")"
	rc4=$?
	accepted="$(printf '%s\n' "$out3" | tail -n1)"
	if [ "$rc4" -eq 0 ] && [ "$accepted" = "$fake_dir/spin" ]; then
		echo "$SELF: FAIL (accepted a PATH spin reporting 6.4.9, below the $SPIN_VERSION_MIN floor)"
		bad_self=1
	elif [ "$rc4" -ne 0 ]; then
		echo "$SELF: FAIL (an old PATH spin was present, but the build fallback exited $rc4 instead of building its own)"
		sed 's/^/        /' "$W/old.err"
		bad_self=1
	else
		echo "    ok: a PATH spin reporting 6.4.9 is rejected -- the bootstrap built its own instead ($accepted)"
	fi

	if [ "$bad_self" -ne 0 ]; then
		echo "$SELF: FAIL"
		return 1
	fi
	echo "$SELF: PASS"
	return 0
}

if [ "${1:-}" = "--self-test" ]; then
	self_test
	exit $?
fi

if [ -n "${1:-}" ]; then
	echo "usage: $0 [--self-test]" >&2
	exit 2
fi

# ---------------------------------------------------------------------------
# The gate: find or build a usable spin, print its path, exit 0. Anything
# else prints a message on stderr and exits non-zero.
# ---------------------------------------------------------------------------

# 1. PATH.
if [ "$SPIN_BOOTSTRAP_IGNORE_PATH" != "1" ] && path_spin="$(command -v spin 2>/dev/null)" && [ -n "$path_spin" ]; then
	if found="$(usable_spin "$path_spin")"; then
		echo "$PROBE: using PATH spin ($found, version >= $SPIN_VERSION_MIN)" >&2
		printf '%s\n' "$found"
		exit 0
	fi
fi

# 2. A prior run of this script.
if found="$(usable_spin "$SPIN_BIN")"; then
	echo "$PROBE: using previously built spin ($found) -- skipping build" >&2
	printf '%s\n' "$found"
	exit 0
fi

# 3. Build it.
build_spin
rc=$?
if [ "$rc" -ne 0 ]; then
	exit "$rc"
fi

if found="$(usable_spin "$SPIN_BIN")"; then
	echo "$PROBE: built and verified spin ($found)" >&2
	printf '%s\n' "$found"
	exit 0
fi

echo "$PROBE: FAIL (built $SPIN_BIN but it did not pass its own usability check)" >&2
exit 1
