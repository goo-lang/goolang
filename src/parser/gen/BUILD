load("@rules_cc//cc:defs.bzl", "cc_library")

# One bison invocation, two outputs.
#
# Makefile:225 needs a GNU Make 4.3 grouped target (`&:`) to express this, and
# its comment records why: without it, `-j` can run bison twice concurrently
# for the two outputs. A genrule with two `outs` is scheduled once by
# construction, so the workaround is not needed here.
#
# bison -d writes the header next to the -o target, so naming parser.tab.c is
# enough to place parser.tab.h beside it.
genrule(
    name = "bison",
    srcs = ["//src/parser:parser.y"],
    outs = [
        "parser.tab.c",
        "parser.tab.h",
    ],
    cmd = "bison -d -o $(location parser.tab.c) $(location //src/parser:parser.y)",
    message = "Running bison on parser.y",
)

# The generated header, on an include path. parser_actions.c and
# lexer_bridge.c both spell it #include "parser.tab.h", so the directory
# containing it must be on the search path rather than the file being passed
# by location.
cc_library(
    name = "parser_tab",
    hdrs = ["parser.tab.h"],
    includes = ["."],
    visibility = ["//src/parser:__pkg__"],
)

# The generated implementation, exposed as a source for //src/parser to
# compile with the rest of the parser's translation units.
filegroup(
    name = "parser_tab_c",
    srcs = ["parser.tab.c"],
    visibility = ["//src/parser:__pkg__"],
)

# ast_free_leak_probe compiles the generated parser with a shell C
# compiler. Under make the two files sit in src/parser/; under Bazel they
# are here, and the probe receives the .c path through PARSER_TAB_C.
filegroup(
    name = "parser_tab_files",
    srcs = ["parser.tab.c", "parser.tab.h"],
    visibility = ["//tests/probes:__pkg__"],
)
