#!/bin/bash
# workflow-targets probe — every `make <target>` that a GitHub Actions workflow
# invokes must exist in the Makefile.
#
# WHY THIS EXISTS. On 2026-08-17 a quarantine pass removed a set of orphan
# Makefile targets after checking that VERIFY_CORE_DEPS and VERIFY_ALL_DEPS did
# not reference them. That check was incomplete: `.github/workflows/demos.yml`
# ran four of them directly. `make verify-core` stayed green on the developer's
# machine and the `demos` job went red on the PR with:
#
#     make: *** No rule to make target 'demo-work-stealing'.  Stop.
#
# The Makefile is not the only consumer of its own targets. A workflow is a
# second one, it lives outside the Makefile, and nothing connected the two.
# This probe is that connection.
#
# THE PARSER MUST BE CONTINUATION-AWARE, and the first version was not. This is
# in tests.yml:
#
#     sudo apt-get install -y --no-install-recommends \
#       make gcc-14 bison llvm-dev clang libblocksruntime-dev
#
# The second line STARTS with the word `make`, but it is a shell continuation
# naming the make PACKAGE. Reading lines independently turned every package
# name into a phantom missing target. So a line continued from a previous line
# ending in a backslash is never treated as a command.

set -u

PROBE="workflow-targets-probe"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

python3 - "$ROOT" "$PROBE" <<'PY'
import pathlib, re, sys

root, probe = pathlib.Path(sys.argv[1]), sys.argv[2]
wf_dir = root / ".github" / "workflows"
if not wf_dir.is_dir():
    print(f"{probe}: SKIPPED (no .github/workflows directory)"); sys.exit(0)

workflows = sorted(list(wf_dir.glob("*.yml")) + list(wf_dir.glob("*.yaml")))
if not workflows:
    print(f"{probe}: SKIPPED (no workflow files)"); sys.exit(0)

# Rule headers in the Makefile: text left of the first ':' that is not ':='.
targets = set()
for line in (root / "Makefile").read_text().split("\n"):
    m = re.match(r'^([A-Za-z0-9_./$()-]+)\s*:(?!=)', line)
    if m:
        targets.add(m.group(1))

def logical_lines(text):
    """Join backslash continuations, so a command is one string.

    This is what makes the apt-get case safe. These four physical lines

        sudo apt-get install -y --no-install-recommends \\
          make gcc-14 bison llvm-dev clang libblocksruntime-dev \\
          valgrind cmake \\
          libjson-c-dev ...

    become ONE logical line beginning `sudo`, so no `make` command is seen and
    no package name is mistaken for a target. It equally repairs the opposite
    case, a genuine `make \\` continued onto its arguments, which a per-line
    reader reported as a target literally named backslash.
    """
    out, buf = [], ""
    for raw in text.split("\n"):
        s = raw.strip()
        if s.endswith("\\"):
            buf += s[:-1].rstrip() + " "
            continue
        out.append(buf + s)
        buf = ""
    if buf:
        out.append(buf)
    return out

failures, checked = [], 0
for wf in workflows:
    for line in logical_lines(wf.read_text()):
        # Split on shell separators so `cd x && make y` is seen.
        for cmd in re.split(r'&&|\|\||;', line):
            cmd = cmd.strip().lstrip("- ")          # YAML "- run:" dash
            cmd = re.sub(r'^run:\s*\|?\s*', '', cmd)
            if not re.match(r'^make(\s|$)', cmd):
                continue
            for word in cmd.split()[1:]:
                if word.startswith("-") or ("=" in word and "/" not in word):
                    continue                        # flag, or a VAR=VALUE override
                # Anything that is not a bare target name ENDS the target list:
                # a redirection, a pipe, a path. `make verify-core 2>&1 | tee
                # verify-core.log` must contribute `verify-core` and nothing
                # else — reading past the redirection invented three phantom
                # targets named `2>&1`, `tee` and `verify-core.log`.
                if not re.fullmatch(r'[A-Za-z0-9_.-]+', word):
                    break
                checked += 1
                if word not in targets:
                    failures.append((wf.name, word))

if failures:
    for name, word in failures:
        print(f"{probe}: FAIL — {name} runs `make {word}`, but the Makefile has no such target.")
    print("  Either restore the target, or update the workflow in the same commit.")
    sys.exit(1)

print(f"{probe}: PASS ({checked} make target(s) across {len(workflows)} workflow file(s) all exist)")
PY
