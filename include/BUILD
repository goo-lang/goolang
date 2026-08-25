load("@rules_cc//cc:defs.bzl", "cc_library")

# The two headers Makefile:23 forces into every translation unit. They are
# declared as hdrs so Bazel treats them as inputs: a -include flag alone would
# compile, but an edit to goo_assert.h would rebuild nothing.
cc_library(
    name = "prelude",
    hdrs = [
        "goo_assert.h",
        "xalloc.h",
    ],
    visibility = ["//visibility:public"],
)

# Every other header in the tree. Split from :prelude so that a target which
# needs a header does not thereby acquire the forced includes.
cc_library(
    name = "headers",
    hdrs = glob(["**/*.h"]),
    includes = ["."],
    visibility = ["//visibility:public"],
)
