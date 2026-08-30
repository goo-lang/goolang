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

# The same, minus the //include:headers blob. See strict_hdrs below.
STRICT_PRELUDE_DEPS = [
    "//include:prelude",
]

def _prelude_deps(strict_hdrs):
    """//include:headers exports all 79 headers, which makes layering_check
    vacuous: no include can fail when one dep supplies everything. A target
    that passes strict_hdrs = True drops it and must name the header libraries
    it actually includes (//include:types_h and friends), which is what gives
    --features=layering_check something to refuse.

    Migrate a package at a time. A target without the flag keeps today's
    behaviour exactly, and is exempt from the check by construction.
    """
    return STRICT_PRELUDE_DEPS if strict_hdrs else PRELUDE_DEPS

def goo_cc_library(name, copts = [], deps = [], strict_hdrs = False, **kwargs):
    """A cc_library with the goolang prelude forced in.

    Args:
      name: target name.
      copts: extra copts, appended to the prelude's.
      deps: libraries this target links.
      strict_hdrs: when True, //include:headers is NOT added and the target
        must declare its own header libraries. Required for layering_check to
        mean anything. See _prelude_deps.
      **kwargs: forwarded to cc_library.
    """
    cc_library(
        name = name,
        copts = PRELUDE_COPTS + copts,
        deps = _prelude_deps(strict_hdrs) + deps,
        **kwargs
    )

def goo_cc_test(name, copts = [], deps = [], strict_hdrs = False, **kwargs):
    """A cc_test with the goolang prelude forced in. See goo_cc_library."""
    cc_test(
        name = name,
        copts = PRELUDE_COPTS + copts,
        deps = _prelude_deps(strict_hdrs) + deps,
        **kwargs
    )
