load("//tools:defs.bzl", "goo_cc_library")

# The escape/release analysis units. Each compiles with no LLVM and no parser,
# which is why they are here in phase 1.
#
# Their TEST suites are NOT here. tests/unit/types/*_test.c all include
# parser.h and call parse_input() and type_check(): they parse real Goo source
# and type-check it before asserting on the analysis. That drags in the
# bison-generated parser, so the four suites are phase 2 work.
#
# They were first sent to phase 3 on the grounds that type_checker.c "has 7
# LLVM references". Every one is inside a // comment. LLVM enters this tree
# through include/codegen.h and include/codegen_cfctx.h only, and reaches the
# 14 files in src/codegen/ plus src/compiler/goo.c -- nothing here.

goo_cc_library(
    name = "release_decision",
    srcs = ["release_decision.c"],
    visibility = ["//visibility:public"],
)

goo_cc_library(
    name = "param_escape",
    srcs = ["param_escape.c"],
    visibility = ["//visibility:public"],
)

goo_cc_library(
    name = "block_escape",
    srcs = ["block_escape.c"],
    visibility = ["//visibility:public"],
)

goo_cc_library(
    name = "local_escape",
    srcs = ["local_escape.c"],
    visibility = ["//visibility:public"],
)

goo_cc_library(
    name = "escape_core",
    srcs = ["escape_core.c"],
    visibility = ["//visibility:public"],
)
