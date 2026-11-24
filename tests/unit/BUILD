load("@rules_cc//cc:defs.bzl", "cc_library")

# The shared assertion and verdict surface for the C unit suites.
#
# orca's testing/check.c is NOT ported over this. goo_check.h carries a third
# outcome orca's does not -- BROKEN (exit 2) when a suite's executed row count
# disagrees with the count it declared -- and that is the outcome that catches
# a table whose rows silently stop executing. Its own comment records the cost
# of not having it: scripts/safety-baseline.txt reached 139 dead entries out of
# 218 before anyone noticed. Replacing it would be a regression.
#
# Header-only: five static inline functions over <stdbool.h> and <stdio.h>.
cc_library(
    name = "goo_check",
    hdrs = ["goo_check.h"],
    includes = ["."],
    visibility = [
        "//tests/unit/runtime:__pkg__",
        "//tests/unit/types:__pkg__",
    ],
)

# goo_check_probe (task 14) reads this as a FILE, through -I with a shell C
# compiler, not as a cc_library dep -- the cc_library above serves the unit
# suites, which link against it instead. ast_free_leak_probe compiles the
# test file with a shell C compiler, the same way.
exports_files(["goo_check.h", "ast/ast_free_leak_test.c"])
