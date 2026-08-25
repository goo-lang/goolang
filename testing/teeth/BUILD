load("//tools:goo_probe.bzl", "goo_expect_probe", "goo_probe")

# Deliberate defects that prove the probe macros can report a failure.
#
# Every target here is tagged "manual", so `bazel test //...` never runs them
# and a red build is never the normal state. tools/verify_probe_teeth.sh runs
# them on purpose and asserts each behaves as declared.
#
# THIS IS THE MOST IMPORTANT CHECK IN PHASE 4. One macro generates roughly 150
# tests. If it cannot fail, all 150 pass while asserting nothing, and nothing
# downstream would notice.

# Same program, two expected files. The control must pass and the defect must
# fail -- a defect that fails in both cases proves the program is broken, not
# that the macro works.
goo_probe(
    name = "probe_control",
    src = "probe_defect.goo",
    expected = "probe_control.expected.txt",
    tags = ["manual"],
)

goo_probe(
    name = "probe_output_defect",
    src = "probe_defect.goo",
    expected = "probe_defect.expected.txt",
    tags = ["manual"],
)

# A program that panics, declared as if it exits cleanly.
goo_expect_probe(
    name = "probe_exit_control",
    src = "probe_exit_defect.goo",
    exit_code = 2,
    tags = ["manual"],
)

goo_expect_probe(
    name = "probe_exit_defect",
    src = "probe_exit_defect.goo",
    exit_code = 0,
    tags = ["manual"],
)

# A diagnostic that never appears.
goo_expect_probe(
    name = "probe_stderr_defect",
    src = "probe_exit_defect.goo",
    exit_code = 2,
    stderr_contains = "THIS STRING NEVER APPEARS",
    tags = ["manual"],
)
