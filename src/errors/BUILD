load("//tools:defs.bzl", "goo_cc_library")

# Makefile:87 ERROR_SRCS.
goo_cc_library(
    name = "errors",
    srcs = ["error.c"],
    visibility = [
        "//src/codegen:__pkg__",
        "//src/parser:__pkg__",
        "//src/types:__pkg__",
        "//tests/unit/types:__pkg__",
    ],
    deps = [
        "//include:error_h",
    ],
)
