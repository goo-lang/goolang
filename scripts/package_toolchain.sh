#!/usr/bin/env bash
# package_toolchain.sh <outdir> — stage a self-contained Goo toolchain and tar it.
#
# The layout is NOT invented here. It is the one the compiler already looks for:
# goo_gooroot_dir() probes <exe-dir>/../lib/goostd, and goo_runtime_archive_path()
# probes <exe-dir>/../lib/libgoo_runtime.a. So a tree of
#
#     <root>/bin/goo
#     <root>/lib/goostd/<pkg>/...
#     <root>/lib/libgoo_runtime.a
#
# needs no environment variable and no wrapper. See ADR 0006.
#
# WHAT SHIPS IS NOT `cp -r goostd/`. goostd/ holds 15 directories and only 9 are
# standard library; the other six (cpkg, fwdref, pkgcheck, kinds, shapes, mypkg)
# are COMPILER TEST FIXTURES that each say so in their header comment. Shipping
# them would make `import "shapes"` resolve for a user. The nine real ones are
# named once, in scripts/check_stdlib_coverage.sh, and read from there rather
# than duplicated.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Two modes, ONE definition of what a toolchain contains:
#   package_toolchain.sh <outdir>          -> a .tar.gz plus its .sha256
#   package_toolchain.sh --stage <prefix>  -> the same layout, written in place
# `make install` uses --stage, so an installed tree and a released tarball can
# never drift apart. That drift is exactly what left `make install` broken.
STAGE_ONLY=0
if [ "${1:-}" = "--stage" ]; then
	STAGE_ONLY=1
	PREFIX="${2:?usage: package_toolchain.sh --stage <prefix>}"
else
	OUT="${1:?usage: package_toolchain.sh <outdir> | --stage <prefix>}"
	mkdir -p "$OUT"
fi

COMPILER="$ROOT/bin/goo"
RUNTIME="$ROOT/lib/libgoo_runtime.a"
for f in "$COMPILER" "$RUNTIME"; do
	[ -f "$f" ] || { echo "package_toolchain: missing $f (run make first)" >&2; exit 1; }
done

# Single source of truth for the stdlib set. An empty extraction would silently
# ship a toolchain with no standard library, so it is a hard error.
PKGS="$(sed -n 's/^GOOSTD_PKG_DIRS="\(.*\)"$/\1/p' "$ROOT/scripts/check_stdlib_coverage.sh")"
[ -n "$PKGS" ] || { echo "package_toolchain: could not read GOOSTD_PKG_DIRS" >&2; exit 1; }

# Capture the WHOLE version token, not just digits and dots. A `[0-9.]*`
# pattern silently truncated "9.9.9-rc1" to "9.9.9", so a pre-release and its
# final would collide on one tarball name. Found by testing with a realistic
# pre-release string instead of a tidy one.
VERSION="$("$COMPILER" version 2>/dev/null | sed -n 's/^Goo Compiler v\([^ ]*\).*/\1/p' | head -1)"
[ -n "$VERSION" ] || { echo "package_toolchain: could not read the compiler version" >&2; exit 1; }
TRIPLE="$(uname -m)-unknown-linux-gnu"
NAME="goo-$VERSION-$TRIPLE"

STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/$NAME/bin" "$STAGE/$NAME/lib/goostd"
cp "$COMPILER" "$STAGE/$NAME/bin/goo"
cp "$RUNTIME"  "$STAGE/$NAME/lib/libgoo_runtime.a"

count=0
for entry in $PKGS; do
	src="${entry#*:}"                      # "strings:goostd/strings" -> goostd/strings
	[ -d "$ROOT/$src" ] || { echo "package_toolchain: missing $src" >&2; exit 1; }
	cp -r "$ROOT/$src" "$STAGE/$NAME/lib/goostd/"
	count=$((count+1))
done
[ "$count" -ge 9 ] || { echo "package_toolchain: only $count stdlib packages staged" >&2; exit 1; }

# The toolchain record, when a container build produced one (ADR 0006 wants a
# published artifact to name what built it). Absent on a native build.
[ -f /etc/goo-toolchain.txt ] && cp /etc/goo-toolchain.txt "$STAGE/$NAME/lib/goo-toolchain.txt"

if [ "$STAGE_ONLY" -eq 1 ]; then
	mkdir -p "$PREFIX/bin" "$PREFIX/lib"
	cp "$STAGE/$NAME/bin/goo" "$PREFIX/bin/goo"
	cp "$STAGE/$NAME/lib/libgoo_runtime.a" "$PREFIX/lib/libgoo_runtime.a"
	rm -rf "$PREFIX/lib/goostd"
	cp -r "$STAGE/$NAME/lib/goostd" "$PREFIX/lib/goostd"
	[ -f "$STAGE/$NAME/lib/goo-toolchain.txt" ] && \
	    cp "$STAGE/$NAME/lib/goo-toolchain.txt" "$PREFIX/lib/goo-toolchain.txt"
	echo "installed: $PREFIX/bin/goo with $count stdlib packages at $PREFIX/lib/goostd"
	exit 0
fi

# Deterministic archive. Every one of these flags removes a source of
# difference: gzip stamps an mtime without -n, and tar records member order,
# mtime, uid and gid. The same class of defect that `ar` had before `ar -D`.
EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" log -1 --format=%ct 2>/dev/null || echo 0)}"
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$EPOCH" \
    -cf - -C "$STAGE" "$NAME" | gzip -n > "$OUT/$NAME.tar.gz"

sha256sum "$OUT/$NAME.tar.gz" | awk '{print $1}' > "$OUT/$NAME.tar.gz.sha256"
echo "packaged: $OUT/$NAME.tar.gz ($count stdlib packages, $(wc -c < "$OUT/$NAME.tar.gz") bytes)"
