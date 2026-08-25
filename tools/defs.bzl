"""Shared C target macros.

Every C target in this repo uses goo_cc_library or goo_cc_test, never a bare
cc_library or cc_test. The forced-include prelude is the reason: a target that
sets the copts by hand and omits the //include:prelude dependency compiles
today and stops rebuilding when goo_assert.h changes. Do not hand-roll them.
"""

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")

# Mirrors Makefile:23. The paths are execroot-relative, which is where Bazel
# runs every action from.
PRELUDE_COPTS = [
    "-include",
    "include/xalloc.h",
    "-include",
    "include/goo_assert.h",
    "-I.",
    "-Iinclude",
    "-D_GNU_SOURCE",
]

PRELUDE_DEPS = [
    "//include:prelude",
    "//include:headers",
]

def goo_cc_library(name, copts = [], deps = [], **kwargs):
    """A cc_library with the goolang prelude forced in."""
    cc_library(
        name = name,
        copts = PRELUDE_COPTS + copts,
        deps = PRELUDE_DEPS + deps,
        **kwargs
    )

def goo_cc_test(name, copts = [], deps = [], **kwargs):
    """A cc_test with the goolang prelude forced in."""
    cc_test(
        name = name,
        copts = PRELUDE_COPTS + copts,
        deps = PRELUDE_DEPS + deps,
        **kwargs
    )
