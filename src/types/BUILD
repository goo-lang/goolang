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

# The escape/release analysis units, with their REAL dependency edges.
#
# Measured with nm on the Make-built objects, 2026-08-25:
#
#   nonretaining      (leaf)
#   escape_core       -> nonretaining
#   param_escape      -> escape_core
#   block_escape      -> escape_core, param_escape
#   local_escape      -> escape_core, param_escape
#   release_decision  -> param_escape, local_escape
#
# Acyclic, and none of it is visible in the Makefile, which links $(SRC_OBJS)
# as one flat set. Phase 1 declared all five as leaves with no deps and put
# nonretaining.c inside :frontend; that linked locally only because Bazel's
# lld path passes --start-lib, which makes archive order irrelevant. CI's
# gcc-14 does not, and reported undefined references to
# goo_callee_is_non_retaining and the local_escape_* family.
#
# None of these compile with LLVM or the parser.

goo_cc_library(
    name = "nonretaining",
    srcs = ["nonretaining.c"],
    visibility = ["//visibility:public"],
)

goo_cc_library(
    name = "escape_core",
    srcs = ["escape_core.c"],
    visibility = ["//visibility:public"],
    deps = [":nonretaining"],
)

goo_cc_library(
    name = "param_escape",
    srcs = ["param_escape.c"],
    visibility = ["//visibility:public"],
    deps = [":escape_core"],
)

goo_cc_library(
    name = "block_escape",
    srcs = ["block_escape.c"],
    visibility = ["//visibility:public"],
    deps = [
        ":escape_core",
        ":param_escape",
    ],
)

goo_cc_library(
    name = "local_escape",
    srcs = ["local_escape.c"],
    visibility = ["//visibility:public"],
    deps = [
        ":escape_core",
        ":param_escape",
    ],
)

goo_cc_library(
    name = "release_decision",
    srcs = ["release_decision.c"],
    visibility = ["//visibility:public"],
    deps = [
        ":local_escape",
        ":param_escape",
    ],
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
        ":nonretaining",
        ":param_escape",
        ":release_decision",
        "//src/ast",
        "//src/comptime",
        "//src/errors",
        "//src/package",
    ],
)
