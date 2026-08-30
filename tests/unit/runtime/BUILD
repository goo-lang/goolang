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
