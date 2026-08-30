load("@rules_cc//cc:defs.bzl", "cc_library")
load("//tools:defs.bzl", "goo_cc_library")

# Makefile:83 AST_SRCS, as two targets rather than one.
#
# Measured with nm on the built objects, 2026-08-30: this package has TWO files
# and ZERO edges between them. Neither ever needed the other, and the single
# coarse target was the only thing saying otherwise. No source changed.
#
# ast.c reaches //src/lexer:token for token_new. That edge was real and
# undeclared before this commit -- the final link resolved it -- which is why
# //src/lexer now declares :token separately.

goo_cc_library(
    name = "ast_impl",
    srcs = ["ast.c"],
    visibility = ["//visibility:private"],
    deps = [
        "//include:ast_h",
        "//src/lexer:token",
    ],
)

goo_cc_library(
    name = "ast_constructors",
    srcs = ["ast_constructors.c"],
    visibility = ["//src/comptime:__pkg__"],
    deps = ["//include:ast_h"],
)

# The name the rest of the tree already uses.
cc_library(
    name = "ast",
    visibility = [
        "//src/codegen:__pkg__",
        "//src/comptime:__pkg__",
        "//src/parser:__pkg__",
        "//src/types:__pkg__",
        "//tests/unit/types:__pkg__",
    ],
    deps = [
        ":ast_constructors",
        ":ast_impl",
    ],
)
