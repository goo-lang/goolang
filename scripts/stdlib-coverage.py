#!/usr/bin/env python3
"""
Go standard-library coverage for the Goo compiler.

Scores the symbols Goo actually supports against the canonical Go API manifest
($GOROOT/api/go1*.txt), by top-level exported symbol (func / type / const /
var), per package. Writes docs/stdlib-coverage.json — the source of truth the
goolang.dev tracker reads — and prints a summary.

Methodology (deliberately conservative — undercount beats overclaim):
  * Denominator: every top-level exported symbol in the Go 1.x API manifest,
    deduped by (package, name) across arch/OS build contexts. Struct fields and
    methods are folded into their type (a type counts once); they are NOT
    separate symbols here, so the unit is "identifiers a user references".
  * Numerator: symbols Goo supports = exported declarations parsed from the
    vendored source in goostd/ (mapped to their real import path) UNION an
    explicit, audited list of symbols served by the C shim. A supported symbol
    counts only if its name matches a real symbol in that package's Go API.

TWO METRICS.
  * "supported" (the JSON `packages[]`, the drift gate): symbols that goostd
    source or the C shim DECLARE. A claim, not evidence.
  * "covered" (gate T-4, scripts/stdlib_api_ratchet.sh): the subset of the
    linux/amd64 API that scripts/check_stdlib_coverage.sh finds a golden
    fixture or a goostd test calling (anti-goal A4). Nothing else counts. Two
    figures, printed as `linux-amd64 <covered>/<total>` and
    `application <covered>/<total>`. The application subset is every package
    except `syscall`, `debug/*`, `go/*`, `image/*` and `runtime/*`.

Denominator: the `$GOROOT/api/go1*.txt` files (not `next/`, `except.txt`), keeping
a line that has no build context or the context linux-amd64 or
linux-amd64-cgo. Go 1.27.4 gives 8,433 symbols, and 3,516 in the application
subset (the design document says 3,513; the 3 symbols are not accounted for).
tests/stdlib/api-linux-amd64.txt is a committed snapshot of that denominator,
so the gate runs where there is no Go (CI).

Run: python3 scripts/stdlib-coverage.py                     (needs `go`; writes the JSON)
     python3 scripts/stdlib-coverage.py --write-snapshot    (also rewrites the snapshot)
     python3 scripts/stdlib-coverage.py --counts            (snapshot only, no Go, no write)
     python3 scripts/stdlib-coverage.py --counts --sole-callers
     python3 scripts/stdlib-coverage.py --check-snapshot    (needs the same Go version)
"""

import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
GOOSTD = REPO / "goostd"
# GOO_STDLIB_COVERAGE_OUT redirects the report, so the drift gate
# (scripts/stdlib_coverage_drift.sh) can regenerate without touching the
# committed file.
OUT = Path(os.environ["GOO_STDLIB_COVERAGE_OUT"]) if os.environ.get(
    "GOO_STDLIB_COVERAGE_OUT") else REPO / "docs" / "stdlib-coverage.json"

# goostd/<dir> -> real Go import path. Test-only packages are excluded.
GOOSTD_DIRMAP = {
    "bits": "math/bits",
    "strconv": "strconv",
    "strings": "strings",
    "utf8": "unicode/utf8",
    # Added 2026-08-17. These four were vendored and made importable, and the
    # verify-core gate (scripts/check_stdlib_coverage.sh, GOOSTD_PKG_DIRS) has
    # asserted they resolve for some time — but this tracker was never told, so
    # it reported "8/173 packages touched" while the gate knew about 9 dirs.
    # Two instruments measuring the same tree must not disagree.
    "bytes": "bytes",
    "io": "io",
    "sort": "sort",
    "filepath": "path/filepath",
    # Nested goostd directories: the key is the path below goostd/.
    "container/list": "container/list",
    "container/ring": "container/ring",
    "encoding": "encoding",
    "structs": "structs",
}
# NOT mapped, deliberately: lanes/ is Goo-specific (no Go counterpart), and
# cpkg, fwdref, kinds, mypkg, pkgcheck, shapes are test fixtures for the
# resolver, not stdlib packages.

# Symbols served by the hardcoded C shim (stdlib_package_lookup). Audited by
# hand against the shim implementation — keep conservative. `fmt.Print` is
# intentionally absent: the package-qualified Print verb is not wired (only
# Println/Printf/Sprintf/Errorf are). strings/strconv also have vendored source
# (parsed below); these are the *additional* shim-only symbols.
SHIM = {
    "fmt": ["Println", "Printf", "Sprintf", "Errorf"],
    "errors": ["New", "Unwrap"],
    "os": ["Args", "Getenv", "Exit"],
    "math": ["Abs", "Max", "Min", "Pow", "Sqrt"],
    "strconv": ["Atoi", "Itoa"],
    "strings": ["Contains", "ToUpper", "ToLower", "Split", "Join", "Index"],
}

GO_VERSION_FALLBACK = "go1.26"

SNAPSHOT = REPO / "tests" / "stdlib" / "api-linux-amd64.txt"
COVERAGE_CHECK = REPO / "scripts" / "check_stdlib_coverage.sh"
# Build contexts that count as linux/amd64. A line with no context holds on
# every platform, so it counts too.
LINUX_AMD64_CTX = {"linux-amd64", "linux-amd64-cgo"}
# Packages outside the application subset (Decision 1 of the design).
NON_APPLICATION = re.compile(r"^(syscall$|debug/|go/|image/|runtime/)")


def goroot():
    env = os.environ.get("GOROOT")
    if env:
        return Path(env)
    try:
        out = subprocess.run(["go", "env", "GOROOT"], capture_output=True, text=True, check=True)
        return Path(out.stdout.strip())
    except Exception:
        sys.exit("error: Go not found. Install Go or set GOROOT so $GOROOT/api/go1*.txt is readable.")


def go_version(root):
    try:
        out = subprocess.run(["go", "version"], capture_output=True, text=True, check=True)
        m = re.search(r"go(\d+\.\d+(?:\.\d+)?)", out.stdout)
        if m:
            return "go" + m.group(1)
    except Exception:
        pass
    return GO_VERSION_FALLBACK


# --- denominator: parse the canonical Go API manifest -----------------------

# feature grammar: "pkg <path>[ (<ctx>)], <feature>"
LINE = re.compile(r"^pkg (\S+)(?: \(([^)]*)\))?, (.*)$")
FUNC = re.compile(r"^func ([A-Z][A-Za-z0-9_]*)")
TYPE = re.compile(r"^type ([A-Z][A-Za-z0-9_]*)")
CONST = re.compile(r"^const ([A-Z][A-Za-z0-9_]*)")
VAR = re.compile(r"^var ([A-Z][A-Za-z0-9_]*)")


def build_canonical(api_dir):
    """package path -> set of exported top-level symbol names, linux/amd64 only."""
    pkgs = {}
    for f in sorted(api_dir.glob("go1*.txt")):
        for raw in f.read_text(errors="replace").splitlines():
            m = LINE.match(raw)
            if not m:
                continue
            path, ctx, feature = m.group(1), m.group(2), m.group(3)
            if ctx and ctx not in LINUX_AMD64_CTX:
                continue
            name = None
            for rx in (FUNC, TYPE, CONST, VAR):
                mm = rx.match(feature)
                if mm:
                    name = mm.group(1)
                    break
            # methods, struct fields, interface members, builtins -> folded away
            if name is None:
                continue
            pkgs.setdefault(path, set()).add(name)
    return pkgs


# --- numerator: symbols Goo supports ----------------------------------------

SRC_FUNC = re.compile(r"^func ([A-Z][A-Za-z0-9_]*)\s*\(")
SRC_TYPE = re.compile(r"^type ([A-Z][A-Za-z0-9_]*)\b")
SRC_CONST = re.compile(r"^const ([A-Z][A-Za-z0-9_]*)\b")
SRC_VAR = re.compile(r"^var ([A-Z][A-Za-z0-9_]*)\b")


def parse_goostd_exports(pkgdir):
    names = set()
    for gofile in pkgdir.glob("*.go"):
        if gofile.name.endswith("_test.go"):
            continue
        for line in gofile.read_text(errors="replace").splitlines():
            for rx in (SRC_FUNC, SRC_TYPE, SRC_CONST, SRC_VAR):
                mm = rx.match(line)
                if mm:
                    names.add(mm.group(1))
                    break
    return names


def build_supported():
    """import path -> set of supported symbol names (claimed, pre-validation)."""
    sup = {}
    for d, path in GOOSTD_DIRMAP.items():
        pkgdir = GOOSTD / d
        if pkgdir.is_dir():
            sup.setdefault(path, set()).update(parse_goostd_exports(pkgdir))
    for path, syms in SHIM.items():
        sup.setdefault(path, set()).update(syms)
    return sup


def classify(supported, total):
    if total == 0:
        return "unknown"
    if supported == 0:
        return "planned"
    if supported >= total:
        return "complete"
    return "partial"


# --- the covered metric (gate T-4) --------------------------------------------

def load_snapshot():
    """package path -> set of names, from the committed denominator snapshot."""
    if not SNAPSHOT.is_file():
        sys.exit(f"error: {SNAPSHOT} not found (run with --write-snapshot)")
    pkgs = {}
    for raw in SNAPSHOT.read_text().splitlines():
        if not raw or raw.startswith("#"):
            continue
        path, name = raw.split("\t")
        pkgs.setdefault(path, set()).add(name)
    return pkgs


def snapshot_text(canonical, version):
    rows = sorted((p, n) for p, names in canonical.items() for n in names)
    return f"# {version} linux-amd64 (go1*.txt, contexts: none, linux-amd64, linux-amd64-cgo)\n" + \
        "".join(f"{p}\t{n}\n" for p, n in rows)


def emitted_coverage():
    """Run check_stdlib_coverage.sh and return its emission lines.

    The script's exit status is NOT read: it is 1 when some shim symbol has no
    caller, which is the smoke gate's business. What this metric needs is the
    list of covered symbols, and an empty list means the measurement broke.
    """
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "covered.tsv"
        r = subprocess.run(["bash", str(COVERAGE_CHECK), "--emit-covered", str(out)],
                           capture_output=True, text=True)
        lines = out.read_text().splitlines() if out.is_file() else []
    rows = [ln.split("\t") for ln in lines if ln]
    if not rows:
        tail = (r.stdout + r.stderr).strip().splitlines()[-3:]
        sys.exit("error: check_stdlib_coverage.sh covered nothing (measurement produced nothing): "
                 + " | ".join(tail))
    return rows


def covered_symbols(canonical, rows):
    """(package, name) -> set of sole-caller markers, for each COUNTED symbol.

    A func, value or type row credits its own name. A goostd method row credits
    its receiver TYPE, because the API manifest folds methods into their type.
    A name that is not in the linux/amd64 API is dropped, so an extra helper
    never inflates the count.
    """
    sym = {}
    for kind, ipath, name, typ, sole in (r + [""] * (5 - len(r)) for r in rows):
        target = typ if kind == "method" else name
        if not target or target not in canonical.get(ipath, ()):
            continue
        sym.setdefault((ipath, target), set()).add(sole)
    return sym


def count_lines(canonical, covered):
    total = {"linux-amd64": 0, "application": 0}
    got = {"linux-amd64": 0, "application": 0}
    for path, names in canonical.items():
        app = not NON_APPLICATION.match(path)
        total["linux-amd64"] += len(names)
        total["application"] += len(names) if app else 0
    for (path, _name) in covered:
        got["linux-amd64"] += 1
        got["application"] += 0 if NON_APPLICATION.match(path) else 1
    return got, total


def print_counts(canonical, covered, sole_callers=False):
    got, total = count_lines(canonical, covered)
    if sole_callers:
        for (path, name), marks in sorted(covered.items()):
            if len(marks) == 1 and "*" not in marks:
                print(f"SOLE\t{path}\t{name}\t{next(iter(marks))}")
    for key in ("linux-amd64", "application"):
        print(f"{key} {got[key]}/{total[key]}")
    return got, total


def cmd_counts(sole_callers):
    canonical = load_snapshot()
    covered = covered_symbols(canonical, emitted_coverage())
    got, _ = print_counts(canonical, covered, sole_callers)
    if got["linux-amd64"] == 0:
        sys.exit("error: no linux/amd64 API symbol is covered (measurement produced nothing)")


def cmd_check_snapshot():
    root = goroot()
    version = go_version(root)
    head = SNAPSHOT.read_text().splitlines()[0] if SNAPSHOT.is_file() else ""
    if not head.startswith(f"# {version} "):
        print(f"snapshot check skipped: snapshot is for '{head[2:].split(' ')[0]}', Go is {version}")
        return
    want = snapshot_text(build_canonical(root / "api"), version)
    if SNAPSHOT.read_text() != want:
        sys.exit(f"error: {SNAPSHOT.relative_to(REPO)} differs from $GOROOT/api; "
                 "run `python3 scripts/stdlib-coverage.py --write-snapshot`")
    print("snapshot matches $GOROOT/api")


def main():
    args = sys.argv[1:]
    if "--counts" in args:
        cmd_counts("--sole-callers" in args)
        return
    if "--check-snapshot" in args:
        cmd_check_snapshot()
        return
    root = goroot()
    api_dir = root / "api"
    if not api_dir.is_dir():
        sys.exit(f"error: {api_dir} not found")

    canonical = build_canonical(api_dir)
    supported = build_supported()
    if "--write-snapshot" in args:
        SNAPSHOT.parent.mkdir(parents=True, exist_ok=True)
        SNAPSHOT.write_text(snapshot_text(canonical, go_version(root)))
    covered = covered_symbols(canonical, emitted_coverage())
    got, total = count_lines(canonical, covered)

    packages = []
    total_syms = sum(len(s) for s in canonical.values())
    supported_syms = 0
    touched = 0

    for path, claimed in sorted(supported.items()):
        canon = canonical.get(path)
        if canon is None:
            # A package Goo names that isn't in the manifest -> report, don't score.
            print(f"warning: '{path}' not found in Go API manifest; skipping", file=sys.stderr)
            continue
        matched = sorted(n for n in claimed if n in canon)
        sup_n = len(matched)
        tot_n = len(canon)
        supported_syms += sup_n
        touched += 1
        packages.append({
            "package": path,
            "status": classify(sup_n, tot_n),
            "supported": sup_n,
            "total": tot_n,
            "pct": round(100.0 * sup_n / tot_n, 1) if tot_n else 0.0,
            "symbols": matched,
        })

    packages.sort(key=lambda p: (-p["pct"], p["package"]))
    overall_pct = round(100.0 * supported_syms / total_syms, 2) if total_syms else 0.0

    result = {
        "go_version": go_version(root),
        "methodology": "top-level exported symbols (func/type/const/var) matched by name against the linux/amd64 lines of $GOROOT/api/go1*.txt; conservative",
        "api_coverage": {
            "note": "symbols a golden fixture or goostd test calls, per scripts/check_stdlib_coverage.sh (gate T-4)",
            "linux_amd64": {"covered": got["linux-amd64"], "total": total["linux-amd64"]},
            "application": {"covered": got["application"], "total": total["application"]},
        },
        "overall": {
            "supported_symbols": supported_syms,
            "total_symbols": total_syms,
            "pct": overall_pct,
            "packages_touched": touched,
            "packages_total": len(canonical),
        },
        "packages": packages,
    }

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(result, indent=2) + "\n")

    o = result["overall"]
    print(f"Go stdlib coverage ({result['go_version']})")
    print(f"  overall: {o['supported_symbols']}/{o['total_symbols']} symbols "
          f"= {o['pct']}%  ({o['packages_touched']}/{o['packages_total']} packages touched)")
    print(f"  linux-amd64 {got['linux-amd64']}/{total['linux-amd64']}")
    print(f"  application {got['application']}/{total['application']}")
    print()
    for p in packages:
        print(f"  {p['status']:9} {p['package']:16} {p['supported']:>3}/{p['total']:<4} {p['pct']:>5}%")
    try:
        shown = OUT.relative_to(REPO)
    except ValueError:
        shown = OUT          # redirected outside the repo by the drift gate
    print(f"\nwrote {shown}")


if __name__ == "__main__":
    main()
