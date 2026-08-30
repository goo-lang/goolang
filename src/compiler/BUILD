load("@rules_cc//cc:defs.bzl", "cc_binary")
load("//tools:defs.bzl", "goo_cc_library")

# Makefile:99 COMPILER_SRCS, split by whether it touches LLVM.
# test_discovery.c does not, so it stays its own library.
goo_cc_library(
    name = "test_discovery",
    srcs = ["test_discovery.c"],
    visibility = ["//visibility:private"],
    deps = [
        "//include:import_resolver_h",
        "//include:test_discovery_h",
    ],
)

# NO -DGOO_VERSION here, deliberately. The Makefile sets it target-specifically
# with `GOO_VERSION ?= 0.1.0`, and goo.c:30-32 carries an #ifndef fallback to
# exactly that same string -- so omitting the define produces an identical
# binary and avoids a fragile nested-quoting copt. If a release ever sets
# GOO_VERSION to something else, this target needs a `defines` entry to match;
# the IR comparison is unaffected either way, because the version does not
# appear in emitted IR.
#
# copts are spelled out rather than using goo_cc_library because that macro
# only wraps cc_library and cc_test.
cc_binary(
    name = "goo",
    srcs = ["goo.c"],
    copts = [
        "-include",
        "include/xalloc.h",
        "-include",
        "include/goo_assert.h",
        "-I.",
        "-Iinclude",
        "-D_GNU_SOURCE",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":test_discovery",
        "//include:ast_h",
        "//include:codegen_h",
        "//include:error_h",
        "//include:import_resolver_h",
        "//include:lexer_h",
        "//include:parser_h",
        "//include:prelude",
        "//include:runtime_h",
        "//include:test_discovery_h",
        "//include:types_h",
        "//src/codegen",
        "//src/types:frontend",
    ],
)
