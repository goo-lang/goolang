load("//tools:defs.bzl", "goo_cc_library")

# VISIBILITY IS THE BOUNDARY, and every value below was measured rather than
# chosen. `bazel query "rdeps(//..., <target>, 1)"`, 2026-08-30:
#
#   private            embedding, escape_core, nonretaining, tc_context,
#                      tc_fctx, types -- no consumer outside this package
#   + unit suites      block_escape, local_escape, param_escape,
#                      release_decision, shim_signatures, terminating_stmt
#   + codegen/compiler frontend, which is the only thing outside src/types
#                      that any other package is allowed to name
#
# Bazel refuses a violation at ANALYSIS time, before anything compiles.
# //testing/layering:visibility_violation is the deliberate defect that proves
# it, and it is tagged manual so `bazel build //...` never expands to it.
#
# Re-measure before widening one. A target that needs to be public is usually a
# target that has grown a second job.

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
    visibility = ["//visibility:private"],
    deps = [
        "//include:nonretaining_h",
        "//include:shim_signatures_h",
    ],
)

goo_cc_library(
    name = "escape_core",
    srcs = ["escape_core.c"],
    visibility = ["//visibility:private"],
    deps = [
        ":nonretaining",
        "//include:escape_core_h",
        "//include:nonretaining_h",
    ],
)

goo_cc_library(
    name = "param_escape",
    srcs = ["param_escape.c"],
    visibility = ["//tests/unit/types:__pkg__"],
    deps = [
        ":escape_core",
        "//include:escape_core_h",
        "//include:nonretaining_h",
        "//include:param_escape_h",
        "//include:token_h",
    ],
)

goo_cc_library(
    name = "block_escape",
    srcs = ["block_escape.c"],
    visibility = ["//tests/unit/types:__pkg__"],
    deps = [
        ":escape_core",
        ":param_escape",
        "//include:block_escape_h",
        "//include:escape_core_h",
        "//include:nonretaining_h",
        "//include:token_h",
    ],
)

goo_cc_library(
    name = "local_escape",
    srcs = ["local_escape.c"],
    visibility = ["//tests/unit/types:__pkg__"],
    deps = [
        ":escape_core",
        ":param_escape",
        "//include:escape_core_h",
        "//include:local_escape_h",
        "//include:nonretaining_h",
        "//include:token_h",
    ],
)

goo_cc_library(
    name = "release_decision",
    srcs = ["release_decision.c"],
    visibility = ["//tests/unit/types:__pkg__"],
    deps = [
        ":local_escape",
        ":param_escape",
        "//include:local_escape_h",
        "//include:param_escape_h",
        "//include:release_decision_h",
        "//include:shim_signatures_h",
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
    visibility = ["//visibility:private"],
    deps = [
        "//include:comptime_h",
        "//include:types_h",
        "//src/comptime",
    ],
)

goo_cc_library(
    name = "terminating_stmt",
    srcs = ["terminating_stmt.c"],
    visibility = ["//tests/unit/types:__pkg__"],
    deps = [
        "//include:types_h",
    ],
)

goo_cc_library(
    name = "tc_fctx",
    srcs = ["tc_fctx.c"],
    visibility = ["//visibility:private"],
    deps = [
        "//include:types_h",
    ],
)

# The TypeChecker accessors, lifted out of type_checker.c so they stop closing
# the cycle. See the file header for the four edges this cuts and for the rule
# about what may live there. Depends on :types and on nothing else in this
# package, which is the property that makes the cut work.
goo_cc_library(
    name = "tc_context",
    srcs = ["tc_context.c"],
    visibility = ["//visibility:private"],
    deps = [
        ":types",
        "//include:types_h",
    ],
)

# The two files the accessor extraction freed. Both were in the seven-file
# cycle, and both left it the moment their single edge into type_checker.c
# became an edge into :tc_context. Measured with nm after the cut: each reaches
# :tc_context and :types and nothing else in this package.
goo_cc_library(
    name = "embedding",
    srcs = ["embedding.c"],
    visibility = ["//visibility:private"],
    deps = [
        ":tc_context",
        ":types",
        "//include:embedding_h",
    ],
)

goo_cc_library(
    name = "shim_signatures",
    srcs = ["shim_signatures.c"],
    visibility = ["//tests/unit/types:__pkg__"],
    deps = [
        ":tc_context",
        ":types",
        "//include:shim_signatures_h",
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
#
# THE REMAINING FIVE ARE MEASURED, not assumed. expression_checker and
# type_checker trade 16 symbols one way and 5 the other, which is the spine of
# the cycle. The thin edges were checked one at a time, 2026-08-30:
#
#   expression_helpers -> type_checker   TWO symbols, and only one is cheap.
#     reject_comptime_function_value calls type_error and type_to_string,
#     both of which are in :types already, so it would move cleanly.
#     check_interface_assign calls report_comptime_method_not_satisfied and
#     type_interface_satisfied, BOTH still in type_checker.c, so moving it
#     re-closes the edge it was meant to cut. Cutting one of the two leaves
#     the edge, so neither is worth taking alone.
#
#   ownership_checker -> expression_checker   type_check_expression, which is
#     the recursive-descent entry point and cannot move.
#
# So this cycle is the type checker's own mutual recursion, not an accessor
# that leaked. The tc_context extraction already took the part that was.
goo_cc_library(
    name = "frontend",
    srcs = [
        "expression_checker.c",
        "expression_helpers.c",
        "lane_ownership.c",
        "ownership_checker.c",
        "type_checker.c",
    ],
    visibility = [
        "//src/codegen:__pkg__",
        "//src/compiler:__pkg__",
        "//tests/unit/types:__pkg__",
    ],
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
        "//include:comptime_h",
        "//include:embedding_h",
        "//include:lane_ownership_h",
        "//include:ownership_checker_h",
        "//include:shim_signatures_h",
        "//include:types_h",
        "//src/ast",
        "//src/comptime",
        "//src/errors",
        "//src/package",
    ],
)
