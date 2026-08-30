load("//tools:defs.bzl", "goo_cc_library")

# Makefile:85 CODEGEN_SRCS. The first target in this repo to take
# @llvm//:llvm_c.
#
# All 15 reach LLVM: 14 include codegen.h or codegen_cfctx.h directly, and
# value_scope.c reaches it through include/value_scope.h. That is the whole
# LLVM surface of the compiler apart from src/compiler/goo.c.
goo_cc_library(
    name = "codegen",
    srcs = [
        "call_codegen.c",
        "cfctx.c",
        "codegen.c",
        "composite_codegen.c",
        "error_union_codegen.c",
        "expression_codegen.c",
        "function_codegen.c",
        "interface_codegen.c",
        "lowlevel_codegen.c",
        "monomorphize.c",
        "nullable_codegen.c",
        "runtime_integration.c",
        "statement_codegen.c",
        "type_mapping.c",
        "value_scope.c",
    ],
    visibility = ["//visibility:public"],
    deps = [
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
