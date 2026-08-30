load("//tools:defs.bzl", "goo_cc_library")

# Makefile:85 CODEGEN_SRCS. The first target in this repo to take
# @llvm//:llvm_c.
#
# All 15 reach LLVM: 14 include codegen.h or codegen_cfctx.h directly, and
# value_scope.c reaches it through include/value_scope.h. That is the whole
# LLVM surface of the compiler apart from src/compiler/goo.c.
# The two helpers that were closing cycles. See codegen_context.c's header for
# why these two and not the other 29 that codegen.c exports.
#
# Measured with nm, 2026-08-30: cfctx.c reached into codegen.c for exactly
# codegen_create_block, and runtime_integration.c for exactly codegen_error.
# Moving the pair frees both files and takes the cycle from 14 to 12.
goo_cc_library(
    name = "codegen_context",
    srcs = ["codegen_context.c"],
    deps = [
        "//include:codegen_h",
        "@llvm//:llvm_c",
    ],
)

# Already a leaf before this change, with no outgoing edge inside the package.
# The coarse target was the only thing hiding that. No source change.
goo_cc_library(
    name = "type_mapping",
    srcs = ["type_mapping.c"],
    deps = [
        "//include:codegen_h",
        "@llvm//:llvm_c",
    ],
)

goo_cc_library(
    name = "cfctx",
    srcs = ["cfctx.c"],
    deps = [
        ":codegen_context",
        "//include:codegen_h",
        "@llvm//:llvm_c",
    ],
)

goo_cc_library(
    name = "runtime_integration",
    srcs = ["runtime_integration.c"],
    deps = [
        ":codegen_context",
        "//include:codegen_h",
        "//include:runtime_h",
        "@llvm//:llvm_c",
    ],
)

goo_cc_library(
    name = "codegen",
    srcs = [
        "call_codegen.c",
        "codegen.c",
        "composite_codegen.c",
        "error_union_codegen.c",
        "expression_codegen.c",
        "function_codegen.c",
        "interface_codegen.c",
        "lowlevel_codegen.c",
        "monomorphize.c",
        "nullable_codegen.c",
        "statement_codegen.c",
        "value_scope.c",
    ],
    visibility = ["//src/compiler:__pkg__"],
    deps = [
        ":cfctx",
        ":codegen_context",
        ":runtime_integration",
        ":type_mapping",
        "//include:block_escape_h",
        "//include:codegen_h",
        "//include:comptime_h",
        "//include:embedding_h",
        "//include:param_escape_h",
        "//include:release_decision_h",
        "//include:runtime_h",
        "//include:types_h",
        "//src/ast",
        "//src/comptime",
        "//src/errors",
        "//src/parser",
        "//src/types:frontend",
        "@llvm//:llvm_c",
    ],
)
