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
