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
        "parity-gate-count.txt",
        "parity.sh",
        "//:Makefile",
    ],
)
