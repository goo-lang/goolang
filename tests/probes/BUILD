load("@rules_shell//shell:sh_test.bzl", "sh_test")

# The probe census must stay current: it decides which macro each of the 186
# probe gates gets, so a stale copy silently mis-routes probes.
#
# no-sandbox because probe_census.sh shells out to parity.sh, which shells out
# to bazel query -- a nested bazel invocation contends on the output-base lock
# inside a sandboxed test. Same reason parity_selftest.sh is tagged this way.
sh_test(
    name = "census_current",
    size = "small",
    srcs = ["census_current.sh"],
    tags = [
        "external",
        "manual",
        "no-sandbox",
    ],
)


# generated.bzl must stay current. It holds 63 targets DERIVED from the
# Makefile's own probe recipes, and its header says "do not edit" -- nothing
# enforced that, so a hand edit would have survived and the derivation would
# have become a snapshot in silence.
#
# NOT tagged manual, unlike census_current above: the generator reads Makefile
# and census.txt through awk, with no nested bazel to deadlock on the
# output-base lock. tests/probes/targets_current.sh --self-test proves it can
# go red, and make runs both.
sh_test(
    name = "targets_current",
    size = "small",
    srcs = ["targets_current.sh"],
    data = [
        "census.txt",
        "generated.bzl",
        "//:Makefile",
        "//tools:gen_probe_targets.py",
    ],
)

load(":generated.bzl", "generated_probes")

# 63 of the 80 fixture-shaped probes, emitted by tools/gen_probe_targets.py.
# The other 17 are omitted with a printed reason -- see the generator's header.
generated_probes()
