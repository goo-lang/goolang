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

# The full front-end type checker: Makefile:119 GOO_TYPES_SRCS, minus the five
# units already declared above as their own libraries, which are picked up as
# deps instead. Splitting them out is deliberate -- it is the fine-grained
# boundary the Makefile's single $(SRC_OBJS) link hides.
#
# No LLVM. Every LLVM identifier in type_checker.c, expression_checker.c and
# shim_signatures.c is inside a // comment; the includes are types.h,
# comptime.h, embedding.h and lane_ownership.h.
goo_cc_library(
    name = "frontend",
    srcs = [
        "embedding.c",
        "expression_checker.c",
        "expression_helpers.c",
        "lane_ownership.c",
        "nonretaining.c",
        "ownership_checker.c",
        "shim_signatures.c",
        "tc_fctx.c",
        "terminating_stmt.c",
        "type_checker.c",
        "types.c",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":block_escape",
        ":escape_core",
        ":local_escape",
        ":param_escape",
        ":release_decision",
        "//src/ast",
        "//src/comptime",
        "//src/errors",
        "//src/package",
    ],
)
