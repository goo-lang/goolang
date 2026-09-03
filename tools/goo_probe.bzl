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
                     needs_compiler = True, needs_cc = False, args = [],
                     env = {}):
    """Run one of the Makefile's script-backed probe gates under Bazel.

    Task 14. These 29 gates are whole shell scripts, not fixture declarations,
    so there is nothing to parse and nothing to generate -- the script IS the
    assertion. What they need is a way to be told where the compiler is: each
    derives `<script-dir>/../bin/goo`, which does not exist in the sandbox.

    The contract is one variable. COMPILER names the binary; the compiler then
    finds its own archive through GOO_RUNTIME, and its stdlib through the
    resolver's working-directory tier, which finds ./goostd in the runfiles
    root. Scripts that touch no compiler pass needs_compiler = False and take
    neither.

    needs_cc = True is a SEPARATE compiler: three gates compile small C
    fixtures with the compiler the Makefile would use
    (`CC_PROBE="$(CC)"`, Makefile:4633). `$(CC)` is a Make variable that only
    resolves with the cc toolchain attached, and it IS Bazel's own C
    compiler -- `/usr/bin/gcc` here, `gcc-14` on CI through the same
    `--repo_env` .github/workflows/bazel.yml already passes. `-std=c23`
    matches the recipes' `CSTD_PROBE`. `$(CC_FLAGS)` is NOT defined for
    `sh_test` (measured) and must not be used.

    args is passed straight through to sh_test. The make gates for
    goo_check and goo_testcase run their script TWICE, the second time with
    --self-test, and the self-test is the teeth -- so it gets a target of
    its own rather than being folded into the first run.

    env is merged INTO the computed env after the contract variables above,
    so a target can add its own. One probe (ast_free_leak) needs the
    generated parser's path, which differs between make and Bazel, and
    $(rootpath) is the only honest way to name a generated file.

    GOO_PROBE_NO_SKIP is set for every probe, with or without a compiler. A
    script that cannot find its tool (valgrind, for instance) prints SKIPPED
    and exits 0 under make, where .github/workflows/tests.yml greps the log
    for that line and fails the job. A Bazel test log is read by nobody, so
    under Bazel the skip must be the failure itself: a script that honours the
    variable turns that branch into a FAIL and a non-zero exit instead.
    """
    computed_env = {"GOO_PROBE_NO_SKIP": "1"}
    probe_data = list(data)
    if needs_compiler:
        # GOOROOT is deliberately NOT set. It names a DIRECTORY, and
        # //goostd:files is a filegroup that $(rootpath) refuses to expand.
        # A probe that imports a vendored package instead adds //goostd:files
        # to its own `data`: the compiler's last resolver tier is ./goostd
        # relative to the working directory, which under Bazel is the
        # runfiles root, so the filegroup being present there is enough.
        computed_env["COMPILER"] = "$(rootpath %s)" % _COMPILER
        computed_env["GOO_RUNTIME"] = "$(rootpath %s)" % _ARCHIVE
        # Fixtures too: these scripts open examples/<name>.goo by NAME, so
        # there is no per-target edge to declare and no way to know which
        # script wants which without auditing 597 files.
        probe_data += [_COMPILER, _ARCHIVE, "//examples:all_fixtures"]

    toolchains = []
    if needs_cc:
        computed_env["CC_PROBE"] = "$(CC)"
        computed_env["CSTD_PROBE"] = "-std=c23"
        toolchains = ["@bazel_tools//tools/cpp:current_cc_toolchain"]

    # The caller's own env is layered on last, so a target can name a
    # variable the contract above does not know about (PARSER_TAB_C, for
    # instance) without having to duplicate COMPILER/GOO_RUNTIME/CC_PROBE.
    computed_env.update(env)

    sh_test(
        name = name,
        srcs = [script],
        args = args,
        data = probe_data,
        env = computed_env,
        size = size,
        tags = tags,
        toolchains = toolchains,
    )
