load("//tools:defs.bzl", "goo_cc_test")

# Phase 1 could not build these: they #include "parser.h" and call
# parse_input() and type_check(), so they parse real Goo source and type-check
# it before asserting on the analysis. They are integration tests of the front
# end wearing the shape of unit tests, and linking $(SRC_OBJS) for all of them
# is what kept that hidden.
goo_cc_test(
    name = "release_decision_test",
    size = "small",
    srcs = ["release_decision_test.c"],
    deps = [
        "//include:ast_h",
        "//include:parser_h",
        "//include:release_decision_h",
        "//include:types_h",
        "//src/parser",
        "//src/types:frontend",
        "//src/types:release_decision",
        "//tests/unit:goo_check",
    ],
)

goo_cc_test(
    name = "param_escape_test",
    size = "small",
    srcs = ["param_escape_test.c"],
    deps = [
        "//include:ast_h",
        "//include:param_escape_h",
        "//include:parser_h",
        "//include:types_h",
        "//src/parser",
        "//src/types:frontend",
        "//src/types:param_escape",
        "//tests/unit:goo_check",
    ],
)

# block_escape_test.c includes param_escape.h as well as block_escape.h.
goo_cc_test(
    name = "block_escape_test",
    size = "small",
    srcs = ["block_escape_test.c"],
    deps = [
        "//include:ast_h",
        "//include:block_escape_h",
        "//include:param_escape_h",
        "//include:parser_h",
        "//include:types_h",
        "//src/parser",
        "//src/types:block_escape",
        "//src/types:frontend",
        "//src/types:param_escape",
        "//tests/unit:goo_check",
    ],
)

# Neither the parser NOR the type checker. The three predicates under test take
# two const char* and consult a static table, so nothing else is needed. This
# only became expressible when the accessor extraction freed shim_signatures.c
# from the seven-file cycle.
#
# THE DEPENDENCY LIST IS PART OF THE TEST, as below. Two names on it is the
# claim.
#
# linkstatic IS LOAD-BEARING, and it records a finding rather than hiding one.
# Bazel's default cc_test link on Linux builds each dep as a shared object and
# resolves every symbol in it. //src/types:shim_signatures reaches :types,
# which reaches //src/comptime for one symbol (comptime_value), and
# libcomptime.so references type_check_expression and type_check_statement --
# both inside the five-file cycle. So src/comptime and src/types are a cycle of
# their own at PACKAGE level, undeclared, and the dynamic link is what says so.
#
# Static linking pulls only the objects the test actually reaches, which is the
# truthful set. It does not fix the package cycle, and that cycle is the next
# thing worth cutting.
goo_cc_test(
    name = "shim_signatures_test",
    size = "small",
    srcs = ["shim_signatures_test.c"],
    linkstatic = True,
    deps = [
        "//include:shim_signatures_h",
        "//src/types:shim_signatures",
        "//tests/unit:goo_check",
    ],
)

# The exception to the comment above, and the point of the exercise.
#
# stmt_is_terminating is a pure structural AST walk, so this suite parses a
# fixture and calls it -- no TypeChecker, no type_check_program. It is the
# first target here that does NOT depend on //src/types:frontend, which is
# only possible because terminating_stmt.c stopped being one of the ten files
# inside that coarse target.
#
# THE DEPENDENCY LIST IS PART OF THE TEST. If the predicate ever acquires a
# TypeChecker dependency, this target stops linking. That is the alarm, and it
# is louder than a comment asking people not to.
goo_cc_test(
    name = "terminating_stmt_test",
    size = "small",
    srcs = ["terminating_stmt_test.c"],
    deps = [
        "//include:ast_h",
        "//include:parser_h",
        "//include:types_h",
        "//src/ast",
        "//src/errors",
        "//src/parser",
        "//src/types:terminating_stmt",
        "//tests/unit:goo_check",
    ],
)

goo_cc_test(
    name = "local_escape_test",
    size = "small",
    srcs = ["local_escape_test.c"],
    deps = [
        "//include:ast_h",
        "//include:local_escape_h",
        "//include:parser_h",
        "//include:types_h",
        "//src/parser",
        "//src/types:frontend",
        "//src/types:local_escape",
        "//tests/unit:goo_check",
    ],
)
