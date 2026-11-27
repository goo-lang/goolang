#!/usr/bin/env bash
# vendor-drift-probe (gates T-3 and T-8 of the trusted-stdlib design).
#
# Every vendored file under goostd/<pkg>/ must equal its pristine twin under
# third_party/go1.27.4-src/<pkg>/ after the ledger goostd/VENDOR.tsv is applied
# to the pristine bytes. Needs no Go toolchain, so it runs in CI.
#
# Ledger columns (tab-separated, no header, sorted with LC_ALL=C):
#   package  file  kind  detail
#   rename    old->new@line:col   (a Goo keyword used as an identifier)
#   swap      asm_file->generic_file
#   drop      file.s
#   override  reason
#
# KNOWN GAP: a package is on the work list if the ledger names it or its pristine
# directory holds a .go file. Deleting the ONLY pristine file of a package that
# has no ledger row removes the package from the list, and this probe cannot see
# it. The API ratchet (T-4) and the goldens of that package catch it later.
#
# VENDOR_ROOT (env) points the probe at another tree. The self-test uses it.
# --self-test builds each fault in a temporary tree, checks that the fault
# changed the input, and requires the probe to FAIL for the named reason.
set -u
export LC_ALL=C
self="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

run_probe() {
    python3 -I - "${VENDOR_ROOT:-$repo}" <<'PYEOF'
import os, re, sys
root = sys.argv[1]
ledger_path = os.path.join(root, "goostd", "VENDOR.tsv")
pris_root = os.path.join(root, "third_party", "go1.27.4-src")
vend_root = os.path.join(root, "goostd")
fails = []
def fail(tag, msg):
    fails.append("vendor-drift-probe: FAIL[%s] %s" % (tag, msg))

rows = []
lines = []
if os.path.exists(ledger_path):
    with open(ledger_path, "rb") as fh:
        lines = fh.read().decode("utf-8").split("\n")
    if lines and lines[-1] == "":
        lines.pop()
if lines != sorted(lines, key=lambda s: s.encode("utf-8")):
    fail("LEDGER", "goostd/VENDOR.tsv is not sorted with LC_ALL=C")
if len(set(lines)) != len(lines):
    fail("LEDGER", "goostd/VENDOR.tsv has a duplicate row")
for n, l in enumerate(lines, 1):
    c = l.split("\t")
    if len(c) != 4 or not all(c):
        fail("LEDGER", "row %d needs four non-empty tab-separated columns: %r" % (n, l))
        continue
    if c[2] not in ("rename", "swap", "drop", "override"):
        fail("LEDGER", "row %d has unknown kind %r" % (n, c[2]))
        continue
    rows.append(c)

pkgs = set(r[0] for r in rows)
for d, _, fs in os.walk(pris_root):
    if any(f.endswith(".go") for f in fs):
        pkgs.add(os.path.relpath(d, pris_root))

if not pkgs:
    fail("EMPTY", "work list is empty: no ledger row and no pristine package")

RENAME = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)->([A-Za-z_][A-Za-z0-9_]*)@(\d+):(\d+)$")
IDENT = re.compile(rb"[A-Za-z0-9_]")
nfiles = nren = 0

def gofiles(d):
    return sorted(f for f in os.listdir(d) if f.endswith(".go")) if os.path.isdir(d) else []

for pkg in sorted(pkgs):
    pdir = os.path.join(pris_root, pkg)
    vdir = os.path.join(vend_root, pkg)
    prows = [r for r in rows if r[0] == pkg]
    pfiles = gofiles(pdir)
    vfiles = gofiles(vdir)
    overrides = set(r[1] for r in prows if r[2] == "override")
    renames = {}
    for r in prows:
        f, kind, det = r[1], r[2], r[3]
        if kind == "rename":
            m = RENAME.match(det)
            if not m:
                fail("LEDGER", "%s/%s: bad rename detail %r" % (pkg, f, det))
                continue
            renames.setdefault(f, []).append((m.group(1), m.group(2), int(m.group(3)), int(m.group(4))))
        elif kind == "drop":
            if det in vfiles:
                fail("LEDGER", "%s: dropped file %s is vendored" % (pkg, det))
        elif kind == "swap":
            if "->" not in det or det.split("->")[0] in vfiles or f not in vfiles:
                fail("LEDGER", "%s: swap row %r does not match the vendored files" % (pkg, det))
    if not pfiles:
        fail("NO-PRISTINE", "%s has ledger rows but no pristine file" % pkg)
        continue
    for f in pfiles:
        if f not in vfiles:
            fail("NO-VENDORED", "%s/%s has a pristine file and no vendored twin" % (pkg, f))
    for f in vfiles:
        if f not in pfiles and f not in overrides:
            fail("NO-PRISTINE-TWIN", "%s/%s is vendored and has no pristine twin" % (pkg, f))
    for f in renames:
        if f not in pfiles:
            fail("NO-PRISTINE", "%s/%s has a rename row and no pristine file" % (pkg, f))
    for f in pfiles:
        if f not in vfiles or f in overrides:
            continue
        data = open(os.path.join(pdir, f), "rb").read()
        starts = [0]
        for i, ch in enumerate(data):
            if ch == 10:
                starts.append(i + 1)
        edits = []
        for old, new, line, col in renames.get(f, []):
            o = old.encode()
            ok = line - 1 < len(starts)
            off = starts[line - 1] + col - 1 if ok else -1
            ok = ok and data[off:off + len(o)] == o \
                and not (off > 0 and IDENT.match(data[off - 1:off])) \
                and not IDENT.match(data[off + len(o):off + len(o) + 1] or b" ")
            if not ok:
                fail("RENAME", "%s/%s: row %s->%s@%d:%d does not match the pristine file" % (pkg, f, old, new, line, col))
                continue
            edits.append((off, o, new.encode()))
        for off, o, nb in sorted(edits, reverse=True):
            data = data[:off] + nb + data[off + len(o):]
        nren += len(edits)
        nfiles += 1
        if data != open(os.path.join(vdir, f), "rb").read():
            fail("DRIFT", "%s/%s differs from the pristine file plus the ledger" % (pkg, f))

if fails:
    print("\n".join(fails))
    print("vendor-drift-probe: FAIL (%d problems, %d packages)" % (len(fails), len(pkgs)))
    sys.exit(1)
print("vendor-drift-probe: PASS (%d packages, %d files, %d renames)" % (len(pkgs), nfiles, nren))
PYEOF
}

tree_hash() {
    (cd "$1" && find . -type f -print0 | sort -z | xargs -0 -r sha256sum | sha256sum | cut -d' ' -f1)
}

make_synth_tree() {
    local t="$1"
    mkdir -p "$t/goostd/synth/plain" "$t/goostd/synth/renamed" \
        "$t/third_party/go1.27.4-src/synth/plain" "$t/third_party/go1.27.4-src/synth/renamed"
    printf 'package plain\n\nfunc F() int { return 1 }\n' > "$t/third_party/go1.27.4-src/synth/plain/plain.go"
    cp "$t/third_party/go1.27.4-src/synth/plain/plain.go" "$t/goostd/synth/plain/plain.go"
    printf 'package renamed\n\nfunc G(match int) int {\n\t// match stays in a comment\n\treturn match + 1\n}\n' \
        > "$t/third_party/go1.27.4-src/synth/renamed/r.go"
    printf 'package renamed\n\nfunc G(match_ int) int {\n\t// match stays in a comment\n\treturn match_ + 1\n}\n' \
        > "$t/goostd/synth/renamed/r.go"
    printf 'synth/renamed\tr.go\trename\tmatch->match_@3:8\nsynth/renamed\tr.go\trename\tmatch->match_@5:9\n' \
        > "$t/goostd/VENDOR.tsv"
}

self_test() {
    local fails=0 before after out rc
    base=""; work=""
    base="$(mktemp -d)"; make_synth_tree "$base"
    trap 'rm -rf "$base" "$work"' EXIT

    # expect_green <label> <root>
    expect_green() {
        out="$(VENDOR_ROOT="$2" bash "$self" 2>&1)"; rc=$?
        if [ $rc -eq 0 ]; then echo "  ok   $1: GREEN"; else echo "  FAIL $1: expected GREEN, got rc=$rc: $(echo "$out" | tail -1)"; fails=$((fails+1)); fi
    }
    # fault <label> <expected tag> <mutation function>
    fault() {
        local label="$1" tag="$2" mut="$3"
        work="$(mktemp -d)"; cp -a "$base/." "$work/"
        before="$(tree_hash "$work")"
        "$mut" "$work"
        after="$(tree_hash "$work")"
        if [ "$before" = "$after" ]; then
            echo "  FAIL $label: the mutation did not change the input"; fails=$((fails+1)); rm -rf "$work"; return
        fi
        out="$(VENDOR_ROOT="$work" bash "$self" 2>&1)"; rc=$?
        if [ $rc -eq 1 ] && echo "$out" | grep -q "FAIL\[$tag\]"; then
            echo "  ok   $label: RED ($(echo "$out" | grep -m1 "FAIL\[$tag\]"))"
        else
            echo "  FAIL $label: expected RED with $tag, got rc=$rc: $(echo "$out" | tail -1)"; fails=$((fails+1))
        fi
        rm -rf "$work"
    }
    m_byte()      { printf ' ' >> "$1/goostd/synth/plain/plain.go"; }
    m_rowcol()    { sed -i 's/@5:9/@5:10/' "$1/goostd/VENDOR.tsv"; }
    m_unrenamed() { cp "$1/third_party/go1.27.4-src/synth/renamed/r.go" "$1/goostd/synth/renamed/r.go"; }
    m_nopristine(){ rm "$1/third_party/go1.27.4-src/synth/renamed/r.go"; }
    m_novendored(){ rm "$1/goostd/synth/plain/plain.go"; }
    m_extra()     { printf 'package plain\n' > "$1/goostd/synth/plain/extra.go"; }
    m_empty()     { rm -rf "$1/third_party/go1.27.4-src/synth" "$1/goostd/synth"; : > "$1/goostd/VENDOR.tsv"; }

    echo "vendor-drift-probe --self-test"
    expect_green "control (synthetic tree, one rename pair)" "$base"
    expect_green "control (unchanged repository tree)" "$repo"
    fault "one changed byte, no ledger row"           DRIFT           m_byte
    fault "rename row with no matching rename (col)"  RENAME          m_rowcol
    fault "rename row, vendored file not renamed"     DRIFT           m_unrenamed
    fault "pristine file missing"                     NO-PRISTINE     m_nopristine
    fault "vendored twin missing"                     NO-VENDORED     m_novendored
    fault "vendored file with no pristine twin"       NO-PRISTINE-TWIN m_extra
    fault "empty work list"                           EMPTY           m_empty
    if [ $fails -eq 0 ]; then echo "vendor-drift-probe --self-test: PASS"; return 0; fi
    echo "vendor-drift-probe --self-test: FAIL ($fails)"; return 1
}

if [ "${1:-}" = "--self-test" ]; then self_test; exit $?; fi
VENDOR_ROOT="${VENDOR_ROOT:-$repo}" run_probe
