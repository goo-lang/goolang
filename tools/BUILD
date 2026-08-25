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
