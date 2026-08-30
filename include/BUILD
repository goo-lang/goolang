load("@rules_cc//cc:defs.bzl", "cc_library")

# HEADERS ARE TARGETS, so that --features=layering_check can refuse something.
#
# The measurement that forced this, 2026-08-30: with a single :headers blob
# exporting all 79 headers to every target, `bazel build //... --config=clang
# --features=layering_check` reported 0 violations. Not because the code is
# clean -- because nothing CAN violate a dep that exports everything. A gate
# that cannot fail is indistinguishable from one that passes.
#
# TEETH ARE CLANG-ONLY, also measured. The gcc toolchain does not declare the
# feature and Bazel drops --features=layering_check without a word, so the
# defect in //testing/layering builds clean under the default config. Run the
# check under --config=clang or do not claim it ran.
#
# GRANULARITY. One target per header where the src/ decomposition made the
# distinction real, and one target per group where the headers are genuinely
# coupled: codegen.h and codegen_cfctx.h include each other, so they cannot be
# separated, and the runtime trio is one unit. The deps below are each header's
# own #include list, read from the file rather than assumed.
#
# NOT REPRESENTED HERE: contracts.h, dependent_types.h, panic_free.h and
# runtime_optimization.h. Each is included only by a .c file that no Bazel
# target compiles -- the probe-only set named in CLAUDE.md -- so none of them
# constrains this graph. Nothing declares them, and nothing needs to.
#
# THERE IS NO BLOB. //include:headers was deleted once its last consumer was
# migrated. Do not add one back: a dep that exports every header makes
# layering_check unable to refuse anything, which is where this started.

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

# ---------------------------------------------------------------------------
# Leaves
# ---------------------------------------------------------------------------

cc_library(
    name = "token_h",
    hdrs = ["token.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "error_h",
    hdrs = ["errors/error.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
)

# platform.h is NOT here. It lives in src/runtime/ beside its own .c file, and
# the old glob(["**/*.h"]) never picked it up either -- declaring it here was a
# missing input, which is how the strict migration found it.
cc_library(
    name = "runtime_h",
    hdrs = [
        "far_transport.h",
        "runtime.h",
    ],
    includes = ["."],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "import_resolver_h",
    hdrs = ["import_resolver.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
)

# ---------------------------------------------------------------------------
# Front end
# ---------------------------------------------------------------------------

cc_library(
    name = "ast_h",
    hdrs = ["ast.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [":token_h"],
)

cc_library(
    name = "lexer_h",
    hdrs = ["lexer.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [":token_h"],
)

cc_library(
    name = "parser_h",
    hdrs = [
        "parser.h",
        "parser/parser_actions.h",
        "parser/parser_errors.h",
    ],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [
        ":ast_h",
        ":error_h",
        ":token_h",
    ],
)

cc_library(
    name = "types_h",
    hdrs = ["types.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [":ast_h"],
)

cc_library(
    name = "comptime_h",
    hdrs = ["comptime.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [
        ":ast_h",
        ":types_h",
    ],
)

cc_library(
    name = "test_discovery_h",
    hdrs = ["test_discovery.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [":ast_h"],
)

# ---------------------------------------------------------------------------
# The type checker's own headers, one per library in //src/types. This is the
# granularity that makes layering_check say something about the decomposition:
# terminating_stmt.c includes types.h and nothing else, and a future edit that
# reaches for escape_core.h is now a build error rather than a review comment.
# ---------------------------------------------------------------------------

cc_library(
    name = "embedding_h",
    hdrs = ["embedding.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [":types_h"],
)

cc_library(
    name = "shim_signatures_h",
    hdrs = ["shim_signatures.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [":types_h"],
)

cc_library(
    name = "ownership_checker_h",
    hdrs = ["ownership_checker.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [":types_h"],
)

cc_library(
    name = "lane_ownership_h",
    hdrs = ["lane_ownership.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [":types_h"],
)

cc_library(
    name = "nonretaining_h",
    hdrs = ["nonretaining.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [":ast_h"],
)

cc_library(
    name = "escape_core_h",
    hdrs = ["escape_core.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [":ast_h"],
)

cc_library(
    name = "param_escape_h",
    hdrs = ["param_escape.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [":ast_h"],
)

cc_library(
    name = "block_escape_h",
    hdrs = ["block_escape.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [
        ":ast_h",
        ":param_escape_h",
    ],
)

cc_library(
    name = "local_escape_h",
    hdrs = ["local_escape.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [
        ":ast_h",
        ":escape_core_h",
        ":param_escape_h",
    ],
)

cc_library(
    name = "release_decision_h",
    hdrs = ["release_decision.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [
        ":ast_h",
        ":escape_core_h",
    ],
)

# ---------------------------------------------------------------------------
# Back end. codegen.h and codegen_cfctx.h include EACH OTHER, so they are one
# target and cannot be otherwise. value_scope.h joins them because it includes
# codegen.h and nothing else does.
# ---------------------------------------------------------------------------

cc_library(
    name = "codegen_h",
    hdrs = [
        "codegen.h",
        "codegen_cfctx.h",
        "value_scope.h",
    ],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = [
        ":ast_h",
        ":runtime_h",
        ":types_h",
    ],
)
