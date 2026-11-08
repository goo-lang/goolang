load("//tools:defs.bzl", "goo_cc_test")

# Makefile json-writer-test. The writer has no dependency beyond libc, so the
# test links only //src/ast:json_writer, the same narrow set the Makefile rule states.
goo_cc_test(
    name = "json_writer_test",
    size = "small",
    srcs = ["json_writer_test.c"],
    deps = [
        "//src/ast:json_writer",
        "//tests/unit:goo_check",
    ],
)
