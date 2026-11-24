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
    visibility = ["//src/ast:__pkg__"],
    deps = ["//include:token_h"],
)

goo_cc_library(
    name = "lexer_impl",
    srcs = ["lexer.c"],
    visibility = ["//visibility:private"],
    deps = [
        ":token",
        "//include:lexer_h",
        "//include:token_h",
    ],
)

# The name the rest of the tree already uses.
cc_library(
    name = "lexer",
    visibility = ["//src/parser:__pkg__"],
    deps = [
        ":lexer_impl",
        ":token",
    ],
)

# The src-scanning probe gates (task 14) read these as FILES: alloc_doors
# greps every .c and .h for a raw strdup, doc_claims counts /proc/self/exe
# sites, and two others compile a named handful with a shell C compiler.
# A glob does not cross a package boundary, so each package names its own.
# allow_empty: most of these ten packages hold only .c files at top level --
# their headers live in include/ -- so the "*.h" half of the pattern matches
# nothing, and the default empty-glob error would otherwise block the build.
filegroup(
    name = "probe_sources",
    srcs = glob(["*.c", "*.h"], allow_empty = True),
    visibility = ["//tests/probes:__pkg__"],
)
