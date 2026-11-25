load("//tools:defs.bzl", "goo_cc_test")

# ast_free_leak_probe compiles this file with a shell C compiler, so it is
# data for //tests/probes, not a cc_test source here.
exports_files(["ast_free_leak_test.c"])

# Makefile json-writer-test. The writer has no dependency beyond libc, so the
# test links only //src/ast:json_writer, the same narrow set the Makefile rule states.
goo_cc_test(
    name = "json_writer_test",
    size = "small",
    srcs = ["json_writer_test.c"],
    deps = [
        "//include:json_writer_h",
        "//src/ast:json_writer",
        "//tests/unit:goo_check",
    ],
)
