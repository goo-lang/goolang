# The grammar is the genrule's input; the generated sources live in the
# sibling package //src/parser/gen. They are NOT generated here on purpose:
# src/parser/parser.tab.c is gitignored but Make writes it to disk, so a
# genrule declaring that name as an output would collide with a real file --
# on developer machines only, never on a clean checkout.
exports_files(["parser.y"])

load("@rules_cc//cc:defs.bzl", "cc_library")
load("//tools:defs.bzl", "goo_cc_library")

# Makefile:82 PARSER_SRCS, as five targets rather than one.
#
# Measured with nm on the built objects, 2026-08-30. The package was four files
# in ONE cycle, and two of the edges closing it carried a single symbol each --
# both of them current_lexer. Moving that variable's definition into
# parser_state.c leaves parser_actions.c and parser_errors.c acyclic, which is
# 1,038 of the package's non-generated lines.
#
# THE REMAINDER IS IRREDUCIBLE. parser.tab.c calls yylex; lexer_bridge.c calls
# yyparse and reads yylval and yylloc. A generated parser and its scanner
# bridge are mutually dependent by construction, so :parser_core holds both and
# always will.

goo_cc_library(
    name = "parser_state",
    srcs = ["parser_state.c"],
    deps = ["//include:lexer_h"],
)

goo_cc_library(
    name = "parser_actions",
    srcs = ["parser_actions.c"],
    deps = [
        ":parser_state",
        "//include:ast_h",
        "//include:lexer_h",
        "//include:parser_h",
        "//include:token_h",
        "//src/ast",
        "//src/parser/gen:parser_tab",
    ],
)

# //src/errors was NOT declared before this split. parser_errors.c reaches it
# for six symbols and the final link resolved them silently.
goo_cc_library(
    name = "parser_errors",
    srcs = ["parser_errors.c"],
    deps = [
        ":parser_state",
        "//include:lexer_h",
        "//include:parser_h",
        "//include:token_h",
        "//src/errors",
    ],
)

# The bison pair, which cannot be separated. parser.tab.c is NOT listed as a
# local source: it comes from //src/parser/gen, which runs bison. The on-disk
# src/parser/parser.tab.c that Make leaves behind is gitignored and is
# deliberately not referenced by anything here.
goo_cc_library(
    name = "parser_core",
    srcs = [
        "lexer_bridge.c",
        "//src/parser/gen:parser_tab_c",
    ],
    deps = [
        ":parser_actions",
        ":parser_errors",
        ":parser_state",
        "//include:ast_h",
        "//include:lexer_h",
        "//include:parser_h",
        "//include:token_h",
        "//src/ast",
        "//src/lexer",
        "//src/parser/gen:parser_tab",
    ],
)

# The name the rest of the tree already uses.
cc_library(
    name = "parser",
    visibility = ["//visibility:public"],
    deps = [
        ":parser_actions",
        ":parser_core",
        ":parser_errors",
        ":parser_state",
    ],
)
