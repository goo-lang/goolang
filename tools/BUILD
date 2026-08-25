load("@rules_shell//shell:sh_test.bzl", "sh_test")
load("//tools:defs.bzl", "goo_cc_test")

goo_cc_test(
    name = "prelude_probe",
    size = "small",
    srcs = ["prelude_probe.c"],
)

# The data list is what makes this work inside the sandbox: the script resolves
# the repo root as $(dirname "$BASH_SOURCE")/.. and reads the Makefile from
# runfiles. Verified 2026-08-25 on a reduced case before it was written here.
sh_test(
    name = "parity_test",
    size = "small",
    srcs = ["parity_test.sh"],
    data = [
        "parity-allowlist.txt",
        "parity-gate-count.txt",
        "parity.sh",
        "//:Makefile",
    ],
)

# Writes a Bazel package into the source tree, so it cannot run inside the
# sandbox. Tagged so `bazel test //...` skips it; CI runs it explicitly, the
# way tools/verify_sanitizers.sh will be run in phase 6. A self-test that never
# runs is indistinguishable from one that passes.
sh_test(
    name = "parity_selftest",
    size = "medium",
    srcs = ["parity_selftest.sh"],
    tags = [
        "external",
        "manual",
        "no-sandbox",
    ],
)

# The grammar conflict baseline, as a Bazel test.
#
# CLAUDE.md and the goo-grammar skill both make a conflict-count delta
# stop-the-line, so this is a gate rather than a report. Before this target
# nothing ran it: no Makefile rule, absent from VERIFY_ALL_DEPS, no workflow.
#
# It runs bison itself, on a scratch copy, and never consumes
# //src/parser/gen -- the baseline must be checked against the grammar as
# written, not against whatever the genrule last produced.
sh_test(
    name = "grammar_tripwire_test",
    size = "small",
    srcs = ["grammar_tripwire_test.sh"],
    data = [
        "//:grammar_tripwire_script",
        "//src/parser:parser.y",
    ],
)

# Compares the Bazel-built compiler against the Make-built one on emitted IR.
#
# Tagged manual and no-sandbox because it needs BOTH build systems, so it can
# never be hermetic and `bazel test //...` must not try to run it. CI runs it
# explicitly, the way it runs parity_selftest.sh.
#
# PHASE 7 DELETES THIS TARGET AND ITS SCRIPT. Once the Makefile is gone the
# question it answers no longer exists.
sh_test(
    name = "compiler_differential",
    size = "large",
    srcs = ["compiler_differential.sh"],
    tags = [
        "external",
        "manual",
        "no-sandbox",
    ],
)

# The golden suite against the Bazel-built compiler and archive.
#
# Tagged manual and no-sandbox: it runs 495 fixtures, each compiling and
# executing a binary, which is neither fast nor sandbox-friendly. CI runs it
# explicitly, the way it runs parity_selftest.sh and compiler_differential.sh.
#
# It asserts 486 passed / 9 failed EXACTLY. The 9 reach the far transport and
# need NNG (phase 3c). When 3c lands, EXPECT_PASS/EXPECT_FAIL become 495/0 and
# this becomes the test-golden gate's Bazel counterpart.
sh_test(
    name = "golden_bazel",
    size = "large",
    srcs = ["golden_bazel.sh"],
    tags = [
        "external",
        "manual",
        "no-sandbox",
    ],
)
