#!/usr/bin/env python3
"""Emit Bazel probe targets from the Makefile's probe recipes.

WHY GENERATED. There are 186 probe gates. Hand-writing 63 target declarations
produces a diff nobody can review, and a hand edit that drifts from its recipe
is invisible. Generating them makes the BUILD file a derivation, and
tests/probes/targets_current.sh gates that it is current.

WHY IT REFUSES TO GUESS. The parse below is an INFERENCE about what a recipe
asserts, and a systematically wrong inference produces confidently green tests
rather than an error. So every recipe that does not match the narrow shape this
understands is OMITTED and printed to stderr with a reason. Silent omission
would be the worst outcome: a gate that looks migrated and is not.

Four probes were already known to need this before it was written:

  asi-gocompat-probe   22 printf cases, not one fixture
  arc-release-probe    24 fixtures in one recipe
  readline-probe       pipes stdin, and uses a .probe_expected.txt sidecar
  osargs-probe         passes argv to the built binary

THE PRINTF CATEGORY works from tests/probes/src/, not from examples/. Those
gates build their sources inside their own recipes, so the sources were
extracted (tools/probe_source_drift.sh) and one gate can own several. Each
SOURCE becomes its own target, and each is refused independently.

The refusal bar is higher there, because a printf recipe asserts per source and
this parse must not emit a target that asserts LESS than the recipe does. A
weaker target is green, and green is what you expect to see. So any assertion
shape not modelled here -- a captured-stdout comparison, a diff, a line count,
a regex alternation -- refuses the source outright.

Usage: gen_probe_targets.py <census-file> > tests/probes/generated.bzl
"""
import os
import re
import subprocess
import sys

MAKEFILE = "Makefile"
SRC_DIR = "tests/probes/src"

# Fixtures that cannot pass until phase 3c brings NNG in.
NNG_FIXTURES = {
    "far_shim_probe", "lanes_allreduce_probe", "lanes_jacobi_probe",
    "lanes_monomorphize_probe", "lanes_partition_probe",
    "lanes_repartition_probe", "lanes_stencil_probe",
    "lanes_stencilstep_probe", "lanes_stencilstep_r2_probe",
}


def recipe(gate):
    out = subprocess.run(
        ["awk", "-v", "p=^%s:" % gate,
         "$0~p{f=1;next} f&&/^[^\\t]/{exit} f", MAKEFILE],
        capture_output=True, text=True)
    return out.stdout


def parse(gate, body):
    """Return (target_dict, None) or (None, reason-for-omission)."""
    if "printf" in body:
        return None, "generates source or input with printf"
    # stdin INTO THE BUILT BINARY only. `wc -l < build/x.actual.txt` is a
    # shell redirect in an assertion, not input to the program under test.
    if re.search(r"\./build/\S+\s*<", body):
        return None, "pipes stdin into the built binary"

    srcs = sorted(set(re.findall(r"examples/([a-z0-9_]+)\.goo", body)))
    if not srcs:
        return None, "compiles no examples/*.goo"
    if len(srcs) > 1:
        return None, "references %d fixtures, not one" % len(srcs)
    src = srcs[0]

    if re.search(r"rc -eq 0|-x build/", body):
        # A compile-reject probe. Extract the diagnostic it demands: the
        # grep -q that is NOT the LLVM-verifier guard.
        msgs = re.findall(r'grep -q[iEF]* +"([^"]+)"', body)
        msgs = [m for m in msgs
                if "Module verification" not in m and "LLVM ERROR" not in m]
        msgs = [m for m in msgs if "|" not in m]  # a regex alternation is not a fixed string
        if len(msgs) != 1:
            return None, "reject probe with %d extractable diagnostics" % len(msgs)
        return {"gate": gate, "name": gate.replace("-", "_"), "src": src,
                "reject": True, "stderr": msgs[0], "nng": False}, None

    exps = sorted(set(re.findall(r"examples/([a-z0-9_.]+\.txt)", body)))
    exps = [e for e in exps if e.endswith("expected.txt")]
    if len(exps) != 1:
        return None, "found %d expected files, need exactly one" % len(exps)
    exp = exps[0]
    if exp != src + ".expected.txt".replace(".goo", ""):
        # accept the standard <src>.expected.txt only
        if exp != "%s.expected.txt" % src:
            return None, "non-standard expected sidecar '%s'" % exp

    # argv passed to the built binary would change what runs. A redirect
    # (`> out.txt`) is not an argument -- the first version of this rule read
    # one as an argument and omitted 48 of 80 probes.
    if re.search(r"\./build/\S+\s+(?![<>|&])\S", body):
        return None, "passes arguments to the built binary"

    return {"gate": gate, "name": gate.replace("-", "_"), "src": src,
            "expected": exp, "reject": False,
            "nng": src in NNG_FIXTURES}, None


def logical_lines(body):
    """Join backslash continuations. A recipe's assertions for one source sit
    on the SAME logical line as its run, and on different physical ones."""
    out, cur = [], ""
    for ln in body.split("\n"):
        cur += ln.rstrip("\\")
        if ln.rstrip().endswith("\\"):
            continue
        out.append(cur)
        cur = ""
    if cur:
        out.append(cur)
    return out


# Assertion shapes this generator does NOT model. Their presence in a source's
# lines refuses that source: emitting anyway would assert less than the recipe.
UNMODELLED = [
    (r'"\$\$out"', "compares captured stdout"),
    (r"\$\$\(\./build/", "captures stdout in a substitution"),
    (r"\bdiff\b", "diffs output against a file"),
    (r"\bwc -l\b", "counts output lines"),
]

# The LLVM-verifier guard is boilerplate in every reject recipe and contains an
# alternation. goo_reject_probe implements that assertion natively, so it must
# be stripped BEFORE the alternation check below -- reading it as an
# unmodelled shape refused 44 sources for having standard boilerplate.
LLVM_GUARD = re.compile(r'grep -q[iEF]* +"[^"]*Module verification[^"]*"')


def _alternation_outside_the_llvm_guard(blob):
    return re.search(r'grep -q[iEF]* +"[^"]*\|', LLVM_GUARD.sub("", blob))


def _diagnostics(lines, stem):
    """Fixed diagnostic strings grepped out of this stem's stderr.

    Returns (messages, saw_case_insensitive). stderr_contains is a
    case-SENSITIVE fixed match, so a recipe's `grep -qiE "error"` cannot be
    expressed here: the real output says "Error". Asserting the case-sensitive
    form instead is a different assertion, so the caller refuses the source.
    """
    msgs, nocase = [], False
    for l in lines:
        if not re.search(r"build/%s\.err" % re.escape(stem), l):
            continue
        for flags, m in re.findall(r'grep -q([iEF]*) +"([^"]+)"', l):
            if "Module verification" in m or "LLVM ERROR" in m:
                continue
            msgs.append(m)
            if "i" in flags:
                nocase = True
    return sorted(set(msgs)), nocase


# stderr_contains is a FIXED-STRING match (grep -qF in tools/run_probe.sh). A
# recipe's `grep -qE "cannot use \[\]int"` is a REGEX, and copying its pattern
# in verbatim asserts something the recipe never asserted. It also breaks
# Starlark, which rejects \[ as an escape -- the loud half of the same problem.
REGEX_META = re.compile(r"[\\\[\](){}*+?|^$]")


def parse_printf_source(gate, lls, stem):
    """Return (target_dict, None) or (None, reason) for ONE extracted source."""
    runs = [l for l in lls if re.search(r"\./build/%s\b" % re.escape(stem), l)]
    comps = [l for l in lls
             if re.search(r"build/%s\.goo" % re.escape(stem), l) and "COMPILER" in l]
    mine = runs + comps
    if not mine:
        return None, "no recipe line mentions this source"

    blob = " ".join(mine)
    for pat, why in UNMODELLED:
        if re.search(pat, blob):
            return None, why
    if _alternation_outside_the_llvm_guard(blob):
        return None, "regex alternation in the diagnostic"

    name = ("%s__%s" % (gate, stem)).replace("-", "_").replace(".", "_")
    msgs, nocase = _diagnostics(mine, stem)

    # A compile-reject source: the compile itself is asserted to fail.
    rejected = any(
        re.search(r"rc -eq 0", l) or re.search(r"-x build/%s\b" % re.escape(stem), l)
        or re.search(r"if \$\(COMPILER\)", l)
        for l in comps)
    if rejected:
        if len(msgs) != 1:
            return None, "reject source with %d extractable diagnostics" % len(msgs)
        if REGEX_META.search(msgs[0]):
            return None, "diagnostic is a regex, not a fixed string"
        return {"printf": True, "name": name, "gate": gate, "stem": stem,
                "reject": True, "stderr": msgs[0], "nocase": nocase}, None

    codes = sorted(set(re.findall(r"rc -ne (\d+)", " ".join(runs))))
    if len(codes) != 1:
        return None, "found %d expected exit codes, need exactly one" % len(codes)
    if len(msgs) > 1:
        return None, "%d diagnostics for one source" % len(msgs)
    if msgs and REGEX_META.search(msgs[0]):
        return None, "diagnostic is a regex, not a fixed string"
    return {"printf": True, "name": name, "gate": gate, "stem": stem,
            "reject": False, "exit": codes[0],
            "stderr": msgs[0] if msgs else None, "nocase": nocase}, None


def parse_printf(gate, body, sources):
    """Yield (target|None, reason, stem) for every extracted source of a gate."""
    lls = logical_lines(body)
    for stem in sources:
        t, why = parse_printf_source(gate, lls, stem)
        yield t, why, stem


def main():
    census = sys.argv[1] if len(sys.argv) > 1 else "tests/probes/census.txt"
    wanted, printf_gates = [], []
    for line in open(census):
        cat, gate = line.split()
        if cat in ("golden", "example"):
            wanted.append(gate)
        elif cat == "printf":
            printf_gates.append(gate)

    targets, omitted = [], []
    for gate in wanted:
        t, why = parse(gate, recipe(gate))
        (targets.append(t) if t else omitted.append((gate, why)))

    # The printf category. One gate owns several sources, and each source is
    # judged on its own -- a gate is never all-or-nothing here.
    n_printf_src = 0
    for gate in printf_gates:
        d = os.path.join(SRC_DIR, gate)
        if not os.path.isdir(d):
            # Refused by the extractor itself; it prints its own reason.
            continue
        # .goo only. The 3 extracted .go files are PACKAGE members of a
        # multi-file fixture, not programs to compile on their own, and a
        # stem taken from one would emit a label for a file that does not
        # exist.
        stems = sorted(f[:-4] for f in os.listdir(d) if f.endswith(".goo"))
        for t, why, stem in parse_printf(gate, recipe(gate), stems):
            n_printf_src += 1
            (targets.append(t) if t else omitted.append(("%s/%s" % (gate, stem), why)))

    print('"""GENERATED by tools/gen_probe_targets.py -- do not edit.')
    print("")
    print("Regenerate with:")
    print("    python3 tools/gen_probe_targets.py tests/probes/census.txt \\")
    print("        > tests/probes/generated.bzl")
    print("")
    print("tests/probes/targets_current.sh gates that this file is current.")
    print('"""')
    print("")
    print('load("//tools:goo_probe.bzl", "goo_expect_probe", "goo_probe", '
          '"goo_reject_probe")')
    print("")
    print("def generated_probes():")
    print('    """%d probes generated from the Makefile."""' % len(targets))
    for t in targets:
        if t.get("printf"):
            label = '"//tests/probes/src:%s/%s.goo"' % (t["gate"], t["stem"])
            if t["reject"]:
                print("    goo_reject_probe(")
                print('        name = "%s",' % t["name"])
                print("        src = %s," % label)
                print('        stderr_contains = "%s",' % t["stderr"].replace('"', '\\"'))
                if t["nocase"]:
                    print("        stderr_ignorecase = True,")
                print("    )")
                continue
            print("    goo_expect_probe(")
            print('        name = "%s",' % t["name"])
            print("        src = %s," % label)
            print("        exit_code = %s," % t["exit"])
            if t["stderr"]:
                print('        stderr_contains = "%s",' % t["stderr"].replace('"', '\\"'))
                if t["nocase"]:
                    print("        stderr_ignorecase = True,")
            print("    )")
            continue
        if t.get("reject"):
            print("    goo_reject_probe(")
            print('        name = "%s",' % t["name"])
            print('        src = "//examples:%s.goo",' % t["src"])
            print('        stderr_contains = "%s",' % t["stderr"].replace('"', '\\"'))
            print("    )")
            continue
        print("    goo_probe(")
        print('        name = "%s",' % t["name"])
        print('        src = "//examples:%s.goo",' % t["src"])
        print('        expected = "//examples:%s",' % t["expected"])
        if t["nng"]:
            print('        # Needs the far transport, so it needs NNG: phase 3c.')
            print('        tags = ["manual"],')
        print("    )")
    if not targets:
        print("    pass")

    for gate, why in omitted:
        print("OMITTED %-28s %s" % (gate, why), file=sys.stderr)
    print("generated %d targets, omitted %d of %d fixture gates + %d printf sources"
          % (len(targets), len(omitted), len(wanted), n_printf_src), file=sys.stderr)


if __name__ == "__main__":
    main()
