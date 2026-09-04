load("//tools:defs.bzl", "goo_cc_library")

# Makefile:83 AST_SRCS.
goo_cc_library(
    name = "ast",
    srcs = [
        "ast.c",
        "ast_constructors.c",
    ],
    visibility = ["//visibility:public"],
)

# Makefile:83 AST_SRCS also lists json_writer.c, but it is its own target here
# because its dependency set is stated, not discovered: the streaming JSON
# writer touches nothing but libc, and json_writer_test links exactly this
# (Makefile json_writer_test rule). Folding it into :ast would drag ast.c's
# lexer reference into that test's link.
goo_cc_library(
    name = "json_writer",
    srcs = ["json_writer.c"],
    visibility = [
        "//src/compiler:__pkg__",
        "//tests/unit/ast:__pkg__",
    ],
)
