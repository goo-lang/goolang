load("//tools:defs.bzl", "goo_cc_test")

# Makefile diagnostic-test. The printer has no dependency beyond libc.
goo_cc_test(
    name = "diagnostic_test",
    size = "small",
    srcs = ["diagnostic_test.c"],
    deps = [
        "//include:diagnostic_h",
        "//src/errors:diagnostic",
        "//tests/unit:goo_check",
    ],
)
