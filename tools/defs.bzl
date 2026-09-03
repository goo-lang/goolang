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

# The forced includes, and nothing else.
#
# There is NO headers blob here, and no way to ask for one. //include:headers
# used to export all 79 headers to every target, which made layering_check
# vacuous -- no include can fail when one dep supplies everything. It was
# deleted once its last consumer was migrated, and the strict_hdrs opt-out went
# with it. A target now names the header libraries it includes, or it does not
# compile. tools/verify_layering.sh asserts the blob has not come back.
PRELUDE_DEPS = [
    "//include:prelude",
]

def goo_cc_library(name, copts = [], deps = [], **kwargs):
    """A cc_library with the goolang prelude forced in.

    deps must name every //include:*_h library the sources include. There is no
    blob to fall back on; see PRELUDE_DEPS.
    """
    cc_library(
        name = name,
        copts = PRELUDE_COPTS + copts,
        deps = PRELUDE_DEPS + deps,
        **kwargs
    )

def goo_cc_test(name, copts = [], deps = [], **kwargs):
    """A cc_test with the goolang prelude forced in. See goo_cc_library."""
    cc_test(
        name = name,
        copts = PRELUDE_COPTS + copts,
        deps = PRELUDE_DEPS + deps,
        **kwargs
    )
