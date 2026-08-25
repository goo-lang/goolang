# The grammar is the genrule's input; the generated sources live in the
# sibling package //src/parser/gen. They are NOT generated here on purpose:
# src/parser/parser.tab.c is gitignored but Make writes it to disk, so a
# genrule declaring that name as an output would collide with a real file --
# on developer machines only, never on a clean checkout.
exports_files(["parser.y"])

load("//tools:defs.bzl", "goo_cc_library")

# Makefile:82 PARSER_SRCS. parser.tab.c is NOT listed as a local source: it
# comes from //src/parser/gen, which runs bison. The on-disk
# src/parser/parser.tab.c that Make leaves behind is gitignored and is
# deliberately not referenced by anything here.
goo_cc_library(
    name = "parser",
    srcs = [
        "lexer_bridge.c",
        "parser_actions.c",
        "parser_errors.c",
        "//src/parser/gen:parser_tab_c",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//src/ast",
        "//src/lexer",
        "//src/parser/gen:parser_tab",
    ],
)
