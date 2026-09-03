load("@rules_cc//cc:defs.bzl", "cc_library")
load("//tools:defs.bzl", "goo_cc_library")

# THIS TARGET'S deps ARE INCOMPLETE, and Bazel cannot let them be complete.
#
# src/comptime and src/types are a cycle at PACKAGE level. Measured with nm on
# the built objects, 2026-08-30: 14 symbols go one way and 13 come back.
#
#   src/types    -> src/comptime   expression_checker, type_checker and types
#                                  reach comptime_eval_expression, the
#                                  comptime_context_* family, comptime_result_
#                                  free and comptime_value_free
#   src/comptime -> src/types      comptime_types and comptime_value reach the
#                                  type_* constructors, plus
#                                  type_check_expression and type_check_statement
#
# So this target cannot declare //src/types:types, which supplies type_new and
# the constructors it plainly uses. The final link resolves them. That is a
# dishonest graph, and writing the reason down is the least that is owed.
#
# TWO SITES CLOSE IT, and each needs an ownership decision rather than a move:
#
#   src/types/types.c:1106      variable_free calls comptime_value_free. One
#                               site. A Variable owns a ComptimeValue, and the
#                               data layer destroys it.
#   src/comptime/comptime_types.c:251,273
#                               calls type_check_expression and
#                               type_check_statement. Comptime asks the checker
#                               to evaluate a type expression.
#
# BOTH must break before the package cycle does. Breaking only the first leaves
# the second, which was verified by walking the edge list rather than assumed.
# Until then //tests/unit/types:shim_signatures_test needs linkstatic, and its
# BUILD comment says why.

# Measured with nm on the built objects, 2026-08-30: this package is four files
# and six internal edges, and only TWO of them are in a cycle. comptime.c and
# comptime_intrinsics.c call each other. The other two never did.
#
#   comptime_value   a leaf inside this package. comptime, comptime_intrinsics
#                    and comptime_types all call INTO it and it calls none of
#                    them.
#   comptime_types   calls comptime and comptime_value, and nothing calls it
#                    back.
#
# No source changed. The cross-package edges above stay undeclared, because
# they are the cycle this file's header describes.

goo_cc_library(
    name = "comptime_value",
    srcs = ["comptime_value.c"],
    deps = [
        "//include:ast_h",
        "//include:comptime_h",
        "//include:token_h",
        "//include:types_h",
        "//src/ast:ast_constructors",
    ],
)

# The two that genuinely call each other: comptime.c reaches
# comptime_intrinsic_emit, _sizeof and _typeof, and comptime_intrinsics.c
# reaches comptime_eval_function_call_enhanced.
goo_cc_library(
    name = "comptime_core",
    srcs = [
        "comptime.c",
        "comptime_intrinsics.c",
    ],
    deps = [
        ":comptime_value",
        "//include:ast_h",
        "//include:comptime_h",
        "//include:token_h",
        "//include:types_h",
        "//src/ast",
    ],
)

# The half of the package cycle that reaches into the type checker, kept on its
# own so the next reader can see exactly which file does it. See the header:
# comptime_types.c:251,273 call type_check_expression and type_check_statement.
goo_cc_library(
    name = "comptime_types",
    srcs = ["comptime_types.c"],
    deps = [
        ":comptime_core",
        ":comptime_value",
        "//include:ast_h",
        "//include:comptime_h",
        "//include:types_h",
    ],
)

# The name the rest of the tree already uses.
cc_library(
    name = "comptime",
    visibility = [
        "//src/codegen:__pkg__",
        "//src/types:__pkg__",
    ],
    deps = [
        ":comptime_core",
        ":comptime_types",
        ":comptime_value",
    ],
)

# The src-scanning probe gates (task 14) read these as FILES: alloc_doors
# greps every .c and .h for a raw strdup, doc_claims counts /proc/self/exe
# sites, and two others compile a named handful with a shell C compiler.
# A glob does not cross a package boundary, so each package names its own.
# allow_empty: most of these ten packages hold only .c files at top level --
# their headers live in include/ -- so the "*.h" half of the pattern matches
# nothing, and the default empty-glob error would otherwise block the build.
filegroup(
    name = "probe_sources",
    srcs = glob(["*.c", "*.h"], allow_empty = True),
    visibility = ["//tests/probes:__pkg__"],
)
