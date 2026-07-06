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

Run: python3 scripts/stdlib-coverage.py   (needs `go` on PATH for $GOROOT)
"""

import json
import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
GOOSTD = REPO / "goostd"
OUT = REPO / "docs" / "stdlib-coverage.json"

# goostd/<dir> -> real Go import path. Test-only packages are excluded.
GOOSTD_DIRMAP = {
    "bits": "math/bits",
    "strconv": "strconv",
    "strings": "strings",
    "utf8": "unicode/utf8",
}

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
LINE = re.compile(r"^pkg (\S+)(?: \([^)]*\))?, (.*)$")
FUNC = re.compile(r"^func ([A-Z][A-Za-z0-9_]*)")
TYPE = re.compile(r"^type ([A-Z][A-Za-z0-9_]*)")
CONST = re.compile(r"^const ([A-Z][A-Za-z0-9_]*)")
VAR = re.compile(r"^var ([A-Z][A-Za-z0-9_]*)")


def build_canonical(api_dir):
    """package path -> set of exported top-level symbol names."""
    pkgs = {}
    for f in sorted(api_dir.glob("go1*.txt")):
        for raw in f.read_text(errors="replace").splitlines():
            m = LINE.match(raw)
            if not m:
                continue
            path, feature = m.group(1), m.group(2)
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


def main():
    root = goroot()
    api_dir = root / "api"
    if not api_dir.is_dir():
        sys.exit(f"error: {api_dir} not found")

    canonical = build_canonical(api_dir)
    supported = build_supported()

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
        "methodology": "top-level exported symbols (func/type/const/var) matched by name against $GOROOT/api/go1*.txt; conservative",
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
    print()
    for p in packages:
        print(f"  {p['status']:9} {p['package']:16} {p['supported']:>3}/{p['total']:<4} {p['pct']:>5}%")
    print(f"\nwrote {OUT.relative_to(REPO)}")


if __name__ == "__main__":
    main()
