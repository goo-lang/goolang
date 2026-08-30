load("//tools:defs.bzl", "goo_cc_library")

# Makefile:97 COMPTIME_SRCS.
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
goo_cc_library(
    name = "comptime",
    srcs = [
        "comptime.c",
        "comptime_intrinsics.c",
        "comptime_types.c",
        "comptime_value.c",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//include:ast_h",
        "//include:comptime_h",
        "//include:token_h",
        "//include:types_h",
        "//src/ast",
    ],
)
