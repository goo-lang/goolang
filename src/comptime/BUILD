load("//tools:defs.bzl", "goo_cc_library")

# Makefile:97 COMPTIME_SRCS.
goo_cc_library(
    name = "comptime",
    srcs = [
        "comptime.c",
        "comptime_intrinsics.c",
        "comptime_types.c",
        "comptime_value.c",
    ],
    strict_hdrs = True,
    visibility = ["//visibility:public"],
    deps = [
        "//include:ast_h",
        "//include:comptime_h",
        "//include:token_h",
        "//include:types_h",
        "//src/ast",
    ],
)
