"""Probe macros: compile a .goo fixture, run it, assert on the result.

Two macros, because the 183 probe gates are not one shape. goo_probe covers
the 80 that compile an examples/*.goo and check stdout. goo_expect_probe
covers the 69 that generate their source and assert an exit code or a
diagnostic -- largely negative tests. They share ONE runner, so the two can
never drift apart on what a fixture means.

The runner in turn mirrors scripts/run_golden.sh's contract deliberately, so
the Make side and the Bazel side agree.
"""

load("@rules_shell//shell:sh_test.bzl", "sh_test")

_COMPILER = "//src/compiler:goo"
_ARCHIVE = "//src/runtime:goo_runtime_archive"
_RUNNER = "//tools:run_probe.sh"

def _probe(name, src, expected, exit_code, stdout_contains, stderr_contains,
           gooflags, timeout_s, size, tags, extra_data):
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
        args += ["--stdout-contains", stdout_contains]
    if stderr_contains:
        args += ["--stderr-contains", stderr_contains]
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
                     timeout_s = 10, size = "small", tags = [], extra_data = []):
    """Compile a fixture and assert an exit code and/or a diagnostic.

    At least one assertion is required. The runner refuses a probe with none,
    because it would compile, run and report PASS while checking nothing.
    """
    if exit_code == None and not stdout_contains and not stderr_contains and not expected:
        fail("goo_expect_probe(%s) asserts nothing" % name)
    _probe(name, src, expected, exit_code, stdout_contains, stderr_contains,
           gooflags, timeout_s, size, tags, extra_data)
