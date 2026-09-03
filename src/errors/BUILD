load("//tools:defs.bzl", "goo_cc_library")

# Makefile:87 ERROR_SRCS.
goo_cc_library(
    name = "errors",
    srcs = ["error.c"],
    visibility = [
        "//src/codegen:__pkg__",
        "//src/parser:__pkg__",
        "//src/types:__pkg__",
        "//tests/unit/types:__pkg__",
    ],
    deps = [
        "//include:error_h",
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
