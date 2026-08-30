load("//tools:goo_probe.bzl", "goo_expect_probe", "goo_probe", "goo_reject_probe")

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

# A program that compiles CLEANLY, declared as a reject. The reject macro must
# refuse it: "the compile failed" is also satisfied by a crash or a typo, so
# the control here is a fixture that does not fail at all.
goo_reject_probe(
    name = "probe_reject_defect",
    src = "probe_defect.goo",
    stderr_contains = "any diagnostic at all",
    tags = ["manual"],
)

# The control: a fixture that IS rejected, with its real diagnostic.
goo_reject_probe(
    name = "probe_reject_control",
    src = "//examples:constint8_reject.goo",
    stderr_contains = "overflows int8",
    tags = ["manual"],
)

# ---------------------------------------------------------------------------
# Sanitizer teeth. Each defect must turn its own config red AND leave the build
# green without it. A defect that fails either way proves only that the code is
# broken, never that the sanitizer saw it.
#
# Plain cc_test, not goo_cc_test: these must not take the goolang prelude,
# which forces xalloc.h in and would replace the very malloc the asan defect
# overflows.
# ---------------------------------------------------------------------------
load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "asan_defect",
    size = "small",
    srcs = ["asan_defect.c"],
    tags = ["manual"],
)

cc_test(
    name = "ubsan_defect",
    size = "small",
    srcs = ["ubsan_defect.c"],
    tags = ["manual"],
)

cc_test(
    name = "tsan_defect",
    size = "small",
    srcs = ["tsan_defect.c"],
    tags = ["manual"],
)
