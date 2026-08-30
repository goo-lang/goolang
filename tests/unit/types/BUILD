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
        "//src/parser",
        "//src/types:block_escape",
        "//src/types:frontend",
        "//src/types:param_escape",
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
        "//src/ast",
        # //src/parser's own edge, which the other four suites get for free
        # through //src/types:frontend. Dropping frontend made it visible:
        # the link named report_error, enter_panic_mode and make_source_location.
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
        "//src/parser",
        "//src/types:frontend",
        "//src/types:local_escape",
        "//tests/unit:goo_check",
    ],
)
