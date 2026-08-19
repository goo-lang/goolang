#!/bin/bash
# archive-determinism probe — lib/libgoo_runtime.a must ship with EVERY
# member's mtime, uid and gid zeroed, and stay that way across a rebuild.
#
# WHAT WENT WRONG (round 1). The recipe piped an MRI script to `ar -M`,
# which stores each member's real mtime, uid, gid and mode. Two builds
# seconds apart produced archives differing by 206 bytes, while all 102
# members extracted byte-identical. `ar -D -M` plus `ranlib -D` fixed the
# members added with `addmod` (our own runtime objects).
#
# WHAT WENT WRONG (round 2). That was not the whole fix. The recipe also
# pulls in the vendored NNG static library with `addlib`, which copies each
# member's header VERBATIM from the source archive instead of regenerating
# it — the outer `-D` never reaches a header it does not write. NNG's own
# CMake build now creates libnng.a with the same `-D` semantics
# (CMAKE_C_ARCHIVE_CREATE/APPEND/FINISH in the $(NNG_LIB) recipe), so the
# `addlib` copy inherits zeroed headers instead of leaking real ones.
#
# WHY THIS PROBE READS THE SHIPPED ARCHIVE'S OWN HEADERS DIRECTLY. An
# earlier version of this probe extracted lib/libgoo_runtime.a's objects and
# re-archived them ITSELF with `ar -D -M` / `ranlib -D`. That tests whether
# the LOCAL ar/ranlib honor `-D` at all — it never reads a single byte of
# the header the shipped file actually carries. Proof: revert `-D` in the
# Makefile's $(RUNTIME_LIB) recipe, force a rebuild, and the 13 `addmod`
# members come back with a real mtime and uid/gid 1000/1000 — but that
# probe still printed PASS, because ITS OWN re-archive step still passed
# `-D` regardless of what the Makefile did. The mutation changed the test,
# not the code under test.
#
# So the primary check below parses lib/libgoo_runtime.a's raw SysV/GNU ar
# member headers (16 name + 12 mtime + 6 uid + 6 gid + 8 mode + 10 size + 2
# magic bytes per member, ar(5)) and asserts every member except the `//`
# extended-name-table entry carries mtime 0 and uid/gid 0/0. This is a
# direct property of the file on disk: no rebuild, no invocation of `ar` to
# interpret it, milliseconds to run, and it catches an `-D` regression in
# EITHER the $(RUNTIME_LIB) recipe or the $(NNG_LIB) recipe feeding it via
# `addlib`.
#
# THE `/` SYMBOL-TABLE MEMBER IS CHECKED, NOT SKIPPED. `ranlib -D` writes
# that member and zeroes its mtime/uid/gid the same as any other; plain
# `ranlib` (no `-D`) writes a real mtime there and nowhere else changes.
# An earlier version of this probe excluded `/` from the check on the same
# line as `//`, so it guarded `ar -D` alone and never noticed a `ranlib -D`
# regression — proven by running plain `ranlib` on the shipped archive: the
# `/` member's mtime became a real timestamp, the archive bytes differed,
# and this probe still printed PASS. `//` stays excluded: its mtime field
# is BLANK, not zero, on every archive this repository has ever produced.
set -u

PROBE="archive-determinism-probe"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LIB="$ROOT/lib/libgoo_runtime.a"

if [ ! -f "$LIB" ]; then
    echo "$PROBE: FAIL (lib/libgoo_runtime.a is missing — run 'make runtime-lib' first)"
    exit 1
fi

header_report="$(python3 - "$LIB" <<'PY'
import sys

path = sys.argv[1]
with open(path, "rb") as f:
    data = f.read()

if not data.startswith(b"!<arch>\n"):
    print("BADMAGIC")
    sys.exit(0)

offset = 8
count = 0
bad = []
while offset + 60 <= len(data):
    header = data[offset:offset + 60]
    name = header[0:16].decode("ascii", "replace").strip()
    mtime = header[16:28].decode("ascii", "replace").strip()
    uid = header[28:34].decode("ascii", "replace").strip()
    gid = header[34:40].decode("ascii", "replace").strip()
    size = header[48:58].decode("ascii", "replace").strip()
    magic = header[58:60]
    if magic != b"\x60\n":
        print(f"BADHEADER@{offset}")
        sys.exit(0)
    member_size = int(size)
    # '//' is the GNU extended-name table; its mtime/uid/gid fields are
    # blank, not zero, so it is excluded. '/' is the ranlib symbol-table
    # member — checked like an ordinary member, see the header comment.
    if name != "//":
        count += 1
        mtime_i = int(mtime) if mtime else -1
        uid_i = int(uid) if uid else -1
        gid_i = int(gid) if gid else -1
        if mtime_i != 0 or uid_i != 0 or gid_i != 0:
            bad.append(f"{name} mtime={mtime_i} uid={uid_i} gid={gid_i}")
    offset += 60 + member_size + (member_size % 2)

print(f"COUNT={count}")
print(f"BADCOUNT={len(bad)}")
for b in bad[:15]:
    print(f"BAD:{b}")
PY
)"

if echo "$header_report" | grep -q '^BADMAGIC'; then
    echo "$PROBE: FAIL (lib/libgoo_runtime.a has no ar(5) magic — not a valid archive)"
    exit 1
fi
if echo "$header_report" | grep -q '^BADHEADER'; then
    echo "$PROBE: FAIL (could not parse an ar member header in lib/libgoo_runtime.a)"
    exit 1
fi

count=$(echo "$header_report" | sed -n 's/^COUNT=//p')
badcount=$(echo "$header_report" | sed -n 's/^BADCOUNT=//p')

if [ -z "$count" ] || [ "$count" -eq 0 ]; then
    echo "$PROBE: FAIL (lib/libgoo_runtime.a has zero ordinary members — the probe would pass vacuously)"
    exit 1
fi

if [ -z "$badcount" ] || [ "$badcount" -ne 0 ]; then
    echo "$PROBE: FAIL — $badcount of $count members in lib/libgoo_runtime.a carry a real mtime/uid/gid."
    echo "$header_report" | grep '^BAD:' | sed 's/^BAD:/  /'
    echo "  Every member's header must show mtime 0, uid 0, gid 0. Check the"
    echo "  Makefile's \$(RUNTIME_LIB) recipe ('ar -D -M', 'ranlib -D') and the"
    echo "  \$(NNG_LIB) recipe's CMAKE_C_ARCHIVE_CREATE/APPEND/FINISH overrides."
    exit 1
fi

# --- Secondary, weaker check: does the LOCAL ar/ranlib honor -D at all? ---
# This extracts the shipped objects and re-archives them ITSELF, so it
# tests the toolchain, not the recipe or the shipped file — see the header
# comment above for why that made an earlier version of this probe blind to
# a real regression. It is informational only and never changes the exit
# status set by the primary check above. Flat extraction also collides on
# any duplicate member basenames (six pairs today, all from NNG), so the
# count it reports is smaller than the primary check's; that is a known,
# accepted limit of THIS secondary check only, not of the probe's verdict.
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/m"
toolchain_status="SKIP (could not extract $LIB)"
if ( cd "$WORK/m" && ar x "$LIB" ) 2>/dev/null; then
    rt_count=$(find "$WORK/m" -name '*.o' | wc -l)
    mri() {
        rm -f "$1"
        { echo "create $1"
          for o in "$WORK"/m/*.o; do echo "addmod $o"; done
          echo "save"; echo "end"; } | ar -D -M
        ranlib -D "$1"
    }
    touch -d '@1000000000' "$WORK"/m/*.o
    mri "$WORK/a1.a"
    touch -d '@2000000000' "$WORK"/m/*.o
    mri "$WORK/a2.a"
    if cmp -s "$WORK/a1.a" "$WORK/a2.a"; then
        toolchain_status="PASS ($rt_count of $count members re-archived by this check, see above for why)"
    else
        toolchain_status="FAIL (local ar/ranlib did not honor -D on a fresh re-archive)"
    fi
fi

echo "$PROBE: PASS ($count members in lib/libgoo_runtime.a all carry mtime 0, uid 0, gid 0)"
echo "  toolchain round-trip (informational only): $toolchain_status"
exit 0
