#!/usr/bin/env python3
"""Vendor one Go standard-library package into goostd/ (trusted-stdlib S0.4).

    python3 tools/vendor_go.py <import path> [--goroot DIR] [--scratch-out DIR]

Steps (design sections 8.2 and 8.3):
  1. refuse unless `go version` prints go1.27.4 (nothing is downloaded);
  2. select files with `go list -json` (GOOS=linux GOARCH=amd64, -tags purego,
     GoFiles only, so no tests);
  3. refuse a package that imports unsafe, reflect, runtime, sync/atomic or an
     internal/... path (stage 1 has no trust boundary);
  4. copy the UNCHANGED files to third_party/go1.27.4-src/<path>/;
  5. rename the Goo keywords match, arena, enum and try used as identifiers
     (tools/vendor_go/main.go, Go's own parser) and write the result to
     goostd/<path>/;
  6. run `go vet` on the renamed copy in a throwaway module, and refuse on
     failure;
  7. rewrite the rows of this package in goostd/VENDOR.tsv.

--scratch-out DIR writes both trees and a ledger under DIR and leaves the
repository alone. Use it to look at a package without vendoring it.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import goo_reserved  # noqa: E402  (one source for the reserved words)

GO_VERSION = "go1.27.4"
FORBIDDEN_EXACT = {"unsafe", "reflect", "runtime", "sync/atomic"}
ASM_BACKED_SUFFIXES = ("_asm.go", "_amd64.go")
GENERIC_SUFFIXES = ("_generic.go", "_noasm.go", "_purego.go")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRISTINE_ROOT = "third_party/go1.27.4-src"


def die(msg):
    print("vendor_go: " + msg, file=sys.stderr)
    sys.exit(1)


def run(cmd, env=None, cwd=None):
    p = subprocess.run(cmd, env=env, cwd=cwd, capture_output=True, text=True)
    return p


def go_env(goroot):
    env = dict(os.environ)
    env.update(GOOS="linux", GOARCH="amd64", GOFLAGS="", GOTOOLCHAIN="local",
               CGO_ENABLED="0", GO111MODULE="on")
    if goroot:
        env["GOROOT"] = goroot
    return env


def forbidden(path):
    return path in FORBIDDEN_EXACT or path == "runtime" or path.startswith("runtime/") \
        or "internal" in path.split("/")


def stem(name):
    for s in ASM_BACKED_SUFFIXES + GENERIC_SUFFIXES:
        if name.endswith(s):
            return name[: -len(s)]
    return None


def swaps(selected, ignored):
    """Pair an ignored asm-backed Go file with a selected generic twin."""
    rows = []
    for a in sorted(ignored):
        if not a.endswith(ASM_BACKED_SUFFIXES):
            continue
        st = stem(a)
        for g in sorted(selected):
            if g.endswith(GENERIC_SUFFIXES) and stem(g) == st:
                rows.append((g, "swap", "%s->%s" % (a, g)))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--goroot", default=None)
    ap.add_argument("--scratch-out", default=None)
    args = ap.parse_args()
    pkg = args.path
    if pkg.startswith("/") or ".." in pkg.split("/"):
        die("bad import path: " + pkg)
    if "internal" in pkg.split("/"):
        die("refused: %s has an internal path element" % pkg)
    env = go_env(args.goroot)
    reserved = goo_reserved.reserved(os.path.join(REPO, "src", "lexer", "token.c"))

    v = run(["go", "version"], env=env)
    if v.returncode != 0 or (GO_VERSION + " ") not in v.stdout + " ":
        die("go version must print %s, got: %s" % (GO_VERSION, (v.stdout + v.stderr).strip()))

    lp = run(["go", "list", "-json", "-tags", "purego", pkg], env=env, cwd=tempfile.gettempdir())
    if lp.returncode != 0:
        die("go list failed: " + lp.stderr.strip())
    info = json.loads(lp.stdout)
    if info.get("Standard") is not True:
        die(pkg + " is not a standard-library package")
    src_dir = info["Dir"]
    selected = info.get("GoFiles", [])
    if not selected:
        die("no GoFiles selected for " + pkg)
    bad = sorted(i for i in info.get("Imports", []) if forbidden(i))
    if bad:
        die("refused: %s imports %s (stage 1 forbids unsafe, reflect, runtime, "
            "sync/atomic and internal/...)" % (pkg, ", ".join(bad)))
    if info.get("CgoFiles"):
        die("refused: %s uses cgo" % pkg)

    if args.scratch_out:
        out_root = os.path.abspath(args.scratch_out)
        ledger_path = os.path.join(out_root, "goostd", "VENDOR.tsv")
        vend_dir = os.path.join(out_root, "goostd", pkg)
        pris_dir = os.path.join(out_root, PRISTINE_ROOT, pkg)
    else:
        ledger_path = os.path.join(REPO, "goostd", "VENDOR.tsv")
        vend_dir = os.path.join(REPO, "goostd", pkg)
        pris_dir = os.path.join(REPO, PRISTINE_ROOT, pkg)

    rows = [("-", "package", GO_VERSION)]
    for s in info.get("SFiles", []):
        rows.append((s, "drop", s))
    rows += swaps(selected, info.get("IgnoredGoFiles", []))

    with tempfile.TemporaryDirectory() as tmp:
        renamed = os.path.join(tmp, "renamed")
        helper = os.path.join(REPO, "tools", "vendor_go", "main.go")
        r = run(["go", "run", helper, "-out", renamed, "-keywords", ",".join(reserved)] + [os.path.join(src_dir, f) for f in selected],
                env=env, cwd=tempfile.gettempdir())
        if r.returncode != 0:
            die("rename helper failed: " + r.stderr.strip())
        bodyless = []
        for line in r.stdout.splitlines():
            f, detail = line.split("\t")
            if detail.startswith("bodyless:"):
                bodyless.append(f + " " + detail[len("bodyless:"):])
                continue
            rows.append((f, "rename", detail))
        # A dropped .s file supplied the body. Without it the package cannot build.
        if bodyless and any(r[1] == "drop" for r in rows):
            die("refused: %s drops assembly files and declares body-less functions: %s"
                % (pkg, ", ".join(bodyless)))

        # go vet on the renamed copy, in a module of its own (T-8).
        mod = os.path.join(tmp, "mod")
        pdir = os.path.join(mod, "vetpkg")
        os.makedirs(pdir)
        with open(os.path.join(mod, "go.mod"), "w") as fh:
            fh.write("module vendorcheck\n\ngo 1.27\n")
        for f in selected:
            shutil.copy(os.path.join(renamed, f), pdir)
        vet = run(["go", "vet", "-tags", "purego", "./..."], env=env, cwd=mod)
        if vet.returncode != 0:
            die("go vet failed on the renamed copy of %s:\n%s" % (pkg, vet.stderr.strip()))

        for d in (vend_dir, pris_dir):
            shutil.rmtree(d, ignore_errors=True)
            os.makedirs(d)
        for f in selected:
            shutil.copy(os.path.join(src_dir, f), pris_dir)
            shutil.copy(os.path.join(renamed, f), vend_dir)

    lic = os.path.join(args.goroot or "/usr/local/go", "LICENSE")
    lic_dst = os.path.join(pris_dir[: -len(pkg)], "LICENSE")
    if os.path.exists(lic) and not os.path.exists(lic_dst):
        shutil.copy(lic, lic_dst)

    # Rewrite this package's rows; keep the others. LC_ALL=C order is byte order.
    kept = []
    if os.path.exists(ledger_path):
        with open(ledger_path, encoding="utf-8") as fh:
            kept = [l.rstrip("\n") for l in fh if l.strip()
                    and (l.split("\t")[0] != pkg or l.split("\t")[2] == "unshipped")]
    new = ["\t".join((pkg,) + r) for r in rows]
    allrows = sorted(set(kept + new), key=lambda s: s.encode("utf-8"))
    os.makedirs(os.path.dirname(ledger_path), exist_ok=True)
    with open(ledger_path, "w", encoding="utf-8") as fh:
        fh.write("".join(l + "\n" for l in allrows))
    print("vendor_go: %s: %d files, %d ledger rows (%s)" % (pkg, len(selected), len(rows), vend_dir))


if __name__ == "__main__":
    main()
