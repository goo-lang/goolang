"""Probe macros: compile a .goo fixture, run it, assert on the result.

Two macros, because the 186 probe gates are not one shape. goo_probe covers
the 80 that compile an examples/*.goo and check stdout. goo_expect_probe
covers the 69 that generate their source and assert an exit code or a
diagnostic -- largely negative tests. They share ONE runner, so the two can
never drift apart on what a fixture means.

The runner in turn mirrors scripts/run_golden.sh's contract deliberately, so
the Make side and the Bazel side agree.
"""

load("@rules_shell//shell:sh_test.bzl", "sh_test")

# Bazel shell-tokenizes sh_test args, so any value containing a space arrives
# as several arguments. A diagnostic like "overflows int8" split into two and
# the runner rejected 'int8' as an unknown argument -- loudly, which is why the
# strict parser is worth having. Every free-text value below is single-quoted.
#
# _shq escapes embedded single quotes with the '\'' idiom, because at least one
# diagnostic is itself quoted -- "Undefined variable 'undefinedFn'" -- and naive
# wrapping closed the quote early.


_COMPILER = "//src/compiler:goo"
_ARCHIVE = "//src/runtime:goo_runtime_archive"
_RUNNER = "//tools:run_probe.sh"

def _shq(v):
    """Single-quote a value for a shell-tokenized sh_test arg."""
    return "'" + v.replace("'", "'\\''") + "'"

def _probe(name, src, expected, exit_code, stdout_contains, stderr_contains,
           gooflags, timeout_s, size, tags, extra_data,
           stderr_ignorecase = False):
    args = [
        "--compiler", "$(rootpath %s)" % _COMPILER,
        "--archive", "$(rootpath %s)" % _ARCHIVE,
        "--src", "$(rootpath %s)" % src,
        "--timeout", str(timeout_s),
    ]
    data = [src, _COMPILER, _ARCHIVE, "//goostd:files"] + extra_data

    if expected:
        args += ["--expected", "$(rootpath %s)" % expected]
        data.append(expected)
    if exit_code != None:
        args += ["--exit", str(exit_code)]
    if stdout_contains:
        args += ["--stdout-contains", _shq(stdout_contains)]
    if stderr_contains:
        args += ["--stderr-contains", _shq(stderr_contains)]
        if stderr_ignorecase:
            args.append("--stderr-ignorecase")
    if gooflags:
        args += ["--gooflags", gooflags]

    sh_test(
        name = name,
        srcs = [_RUNNER],
        args = args,
        data = data,
        size = size,
        tags = tags,
    )

def goo_probe(name, src, expected, exit_code = None, stderr_contains = None,
              gooflags = None, timeout_s = 10, size = "small", tags = [],
              extra_data = []):
    """Compile a fixture, run it, and diff stdout against `expected`."""
    _probe(name, src, expected, exit_code, None, stderr_contains,
           gooflags, timeout_s, size, tags, extra_data)

def goo_expect_probe(name, src, exit_code = None, stdout_contains = None,
                     stderr_contains = None, expected = None, gooflags = None,
                     timeout_s = 10, size = "small", tags = [], extra_data = [],
                     stderr_ignorecase = False):
    """Compile a fixture and assert an exit code and/or a diagnostic.

    At least one assertion is required. The runner refuses a probe with none,
    because it would compile, run and report PASS while checking nothing.
    """
    if exit_code == None and not stdout_contains and not stderr_contains and not expected:
        fail("goo_expect_probe(%s) asserts nothing" % name)
    _probe(name, src, expected, exit_code, stdout_contains, stderr_contains,
           gooflags, timeout_s, size, tags, extra_data, stderr_ignorecase)

def goo_reject_probe(name, src, stderr_contains, gooflags = None,
                     timeout_s = 10, size = "small", tags = [], extra_data = [],
                     stderr_ignorecase = False):
    """A fixture the compiler must REJECT, cleanly and with a named diagnostic.

    Four assertions, and the fourth is why this is not just an exit-code check:
    the compile must fail, no binary may be emitted, the named diagnostic must
    appear, and stderr must NOT carry "Module verification failed" or
    "LLVM ERROR" -- which would mean invalid IR reached the verifier instead of
    the compiler producing a diagnostic. Both give a non-zero exit, and only
    one of them is the language behaving correctly.

    stderr_contains is mandatory. "The compile failed" is also satisfied by a
    crash, a missing file, or a typo in the fixture name.
    """
    if not stderr_contains:
        fail("goo_reject_probe(%s) needs stderr_contains" % name)
    sh_test(
        name = name,
        srcs = [_RUNNER],
        args = [
            "--compiler", "$(rootpath %s)" % _COMPILER,
            "--archive", "$(rootpath %s)" % _ARCHIVE,
            "--src", "$(rootpath %s)" % src,
            "--timeout", str(timeout_s),
            "--reject",
            "--stderr-contains", _shq(stderr_contains),
        ] + (["--stderr-ignorecase"] if stderr_ignorecase else [])
          + (["--gooflags", gooflags] if gooflags else []),
        data = [src, _COMPILER, _ARCHIVE, "//goostd:files"] + extra_data,
        size = size,
        tags = tags,
    )


def goo_script_probe(name, script, data = [], size = "medium", tags = [],
                     needs_compiler = True):
    """Run one of the Makefile's script-backed probe gates under Bazel.

    Task 14. These 29 gates are whole shell scripts, not fixture declarations,
    so there is nothing to parse and nothing to generate -- the script IS the
    assertion. What they need is a way to be told where the compiler is: each
    derives `<script-dir>/../bin/goo`, which does not exist in the sandbox.

    The contract is one variable. COMPILER names the binary; the compiler then
    finds its own archive and stdlib through GOO_RUNTIME and GOOROOT, which it
    already reads. Scripts that touch no compiler pass needs_compiler = False
    and take none of the three.
    """
    env = {}
    probe_data = list(data)
    if needs_compiler:
        # GOOROOT is deliberately NOT set. It names a DIRECTORY, and
        # //goostd:files is a filegroup that $(rootpath) refuses to expand.
        # A probe that imports a vendored package must pass gooroot_file, so
        # the need is declared per probe rather than assumed for all 29.
        env = {
            "COMPILER": "$(rootpath %s)" % _COMPILER,
            "GOO_RUNTIME": "$(rootpath %s)" % _ARCHIVE,
        }
        probe_data += [_COMPILER, _ARCHIVE]

    sh_test(
        name = name,
        srcs = [script],
        data = probe_data,
        env = env,
        size = size,
        tags = tags,
    )
