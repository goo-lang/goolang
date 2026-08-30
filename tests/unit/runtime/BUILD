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
