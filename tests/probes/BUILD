load("@rules_shell//shell:sh_test.bzl", "sh_test")

# The probe census must stay current: it decides which macro each of the 183
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

load("//tools:goo_probe.bzl", "goo_probe")

# The macro's acceptance test: one hand-written probe over a known-good
# fixture. The generated ones come later; this proves the wiring first.
goo_probe(
    name = "switch_probe",
    src = "//examples:switch_probe.goo",
    expected = "//examples:switch_probe.expected.txt",
)
