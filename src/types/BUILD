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

# The three leaves that were inside :frontend, now declared on their own.
#
# Measured with nm on the Bazel-built objects, 2026-08-30. The whole package is
# 16 files and 37 inter-file edges. Seven of them form ONE cycle -- embedding,
# expression_checker, expression_helpers, lane_ownership, ownership_checker,
# shim_signatures, type_checker -- and Bazel cannot express a cycle, so a
# coarse target is the only legal description of those. These three are not in
# it. They have no outgoing edge to any other file in this package at all:
#
#   types             14 libc symbols + comptime_value  (//src/comptime)
#   terminating_stmt   1 libc symbol
#   tc_fctx            1 libc symbol
#
# Nothing here is a source change. The files were always leaves; the coarse
# target was hiding it. What it buys is a handle: a cc_test can only name a
# target that exists, which is why the six acyclic units above have four unit
# suites between them and the ten files inside :frontend had none.
#
# Re-take the measurement before moving a file out of :frontend. A file that
# looks like a leaf in the source can still close a cycle through a symbol the
# reader did not think to look for.

goo_cc_library(
    name = "types",
    srcs = ["types.c"],
    visibility = ["//visibility:public"],
    deps = ["//src/comptime"],
)

goo_cc_library(
    name = "terminating_stmt",
    srcs = ["terminating_stmt.c"],
    visibility = ["//visibility:public"],
)

goo_cc_library(
    name = "tc_fctx",
    srcs = ["tc_fctx.c"],
    visibility = ["//visibility:public"],
)

# The TypeChecker accessors, lifted out of type_checker.c so they stop closing
# the cycle. See the file header for the four edges this cuts and for the rule
# about what may live there. Depends on :types and on nothing else in this
# package, which is the property that makes the cut work.
goo_cc_library(
    name = "tc_context",
    srcs = ["tc_context.c"],
    visibility = ["//visibility:public"],
    deps = [":types"],
)

# The two files the accessor extraction freed. Both were in the seven-file
# cycle, and both left it the moment their single edge into type_checker.c
# became an edge into :tc_context. Measured with nm after the cut: each reaches
# :tc_context and :types and nothing else in this package.
goo_cc_library(
    name = "embedding",
    srcs = ["embedding.c"],
    visibility = ["//visibility:public"],
    deps = [
        ":tc_context",
        ":types",
    ],
)

goo_cc_library(
    name = "shim_signatures",
    srcs = ["shim_signatures.c"],
    visibility = ["//visibility:public"],
    deps = [
        ":tc_context",
        ":types",
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
        "expression_checker.c",
        "expression_helpers.c",
        "lane_ownership.c",
        "ownership_checker.c",
        "type_checker.c",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":block_escape",
        ":embedding",
        ":escape_core",
        ":local_escape",
        ":nonretaining",
        ":param_escape",
        ":release_decision",
        ":shim_signatures",
        ":tc_context",
        ":tc_fctx",
        ":terminating_stmt",
        ":types",
        "//src/ast",
        "//src/comptime",
        "//src/errors",
        "//src/package",
    ],
)
