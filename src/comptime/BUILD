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
    visibility = ["//visibility:public"],
    deps = ["//src/ast"],
)
