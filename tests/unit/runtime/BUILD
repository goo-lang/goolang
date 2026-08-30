load("//tools:defs.bzl", "goo_cc_test")

# The first suite ported, because it is the only one the Makefile already links
# narrowly: obj_header_test takes $(RUNTIME_LIB), where every other suite takes
# $(SRC_OBJS), the whole tree. Its dependency set is stated, not discovered.
goo_cc_test(
    name = "obj_header_test",
    size = "small",
    srcs = ["obj_header_test.c"],
    deps = [
        "//include:runtime_h",
        "//src/runtime",
        "//tests/unit:goo_check",
    ],
)

# The first suite to name //src/runtime:arena. Before the runtime package was
# split, arena.c was one of twelve files in a single target and no test could
# name it, which is why 134 lines of allocator with four exported functions had
# none.
#
# The dependency list is two names plus the header. :arena pulls :core
# transitively for goo_panic and goo_zerobase, which arena.c genuinely uses.
goo_cc_test(
    name = "arena_test",
    size = "small",
    srcs = ["arena_test.c"],
    deps = [
        "//include:runtime_h",
        "//src/runtime:arena",
        "//tests/unit:goo_check",
    ],
)

# The runtime defer stack. Two exported functions, and nothing exercised them
# before the runtime package was split.
#
# RUN THIS UNDER --config=valgrind AS WELL. goo_defer_run frees each entry's
# env and then the entries array, and nothing in-process can observe that
# without an allocation-interception harness this repo does not have. A run
# that stopped freeing shows up under valgrind as a leak and here as nothing.
# The suite allocates every env with goo_alloc so that check is meaningful.
goo_cc_test(
    name = "defer_test",
    size = "small",
    srcs = ["defer_test.c"],
    deps = [
        "//include:runtime_h",
        "//src/runtime:defer",
        "//tests/unit:goo_check",
    ],
)

goo_cc_test(
    name = "io_test",
    size = "small",
    srcs = ["io_test.c"],
    deps = [
        "//include:runtime_h",
        "//src/runtime:io",
        "//tests/unit:goo_check",
    ],
)

goo_cc_test(
    name = "sync_shim_test",
    size = "small",
    srcs = ["sync_shim_test.c"],
    deps = [
        "//include:runtime_h",
        "//src/runtime:sync_shim",
        "//tests/unit:goo_check",
    ],
)

goo_cc_test(
    name = "testing_test",
    size = "small",
    srcs = ["testing_test.c"],
    deps = [
        "//include:runtime_h",
        "//src/runtime:testing",
        "//tests/unit:goo_check",
    ],
)

goo_cc_test(
    name = "time_shim_test",
    size = "small",
    srcs = ["time_shim_test.c"],
    deps = [
        "//include:runtime_h",
        "//src/runtime:time_shim",
        "//tests/unit:goo_check",
    ],
)
