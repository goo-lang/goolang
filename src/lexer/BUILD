load("@rules_cc//cc:defs.bzl", "cc_library")
load("//tools:defs.bzl", "goo_cc_library")

# Makefile:81 LEXER_SRCS, as two targets rather than one.
#
# Measured with nm on the built objects, 2026-08-30: token.c is a leaf, and
# lexer.c reaches it for exactly three symbols -- token_is_keyword,
# token_keyword_type and token_new. Acyclic, so Bazel can say so. No source
# changed.
#
# :token exists on its own because //src/ast needs it. ast.c calls token_new
# and had no declared edge for it; the final link resolved it silently.

goo_cc_library(
    name = "token",
    srcs = ["token.c"],
    visibility = ["//visibility:public"],
    deps = ["//include:token_h"],
)

goo_cc_library(
    name = "lexer_impl",
    srcs = ["lexer.c"],
    visibility = ["//visibility:public"],
    deps = [
        ":token",
        "//include:lexer_h",
        "//include:token_h",
    ],
)

# The name the rest of the tree already uses.
cc_library(
    name = "lexer",
    visibility = ["//visibility:public"],
    deps = [
        ":lexer_impl",
        ":token",
    ],
)
