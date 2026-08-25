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
