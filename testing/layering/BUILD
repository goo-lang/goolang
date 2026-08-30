load("@rules_cc//cc:defs.bzl", "cc_library")

# Deliberate defects that prove the two boundary gates can refuse something.
# Nothing here ships. Every defect target is tagged "manual", so
# `bazel build //...` never expands to it and red is never the normal state.
#
# LAYERING, measured 2026-08-30. --features=layering_check refuses a target
# that includes a header reachable only through a transitive dep. It has teeth
# under CLANG ONLY:
#
#   --config=clang   :violation fails, :ok builds        HAS TEETH
#   default (gcc)    :violation BUILDS                   silently ignored
#
# The gcc toolchain does not declare the feature, so Bazel drops the request
# without a word. An exit 0 from `--features=layering_check` is therefore not
# evidence of anything on its own.
#
# AND IT IS CURRENTLY VACUOUS ON THE REAL CODE. `bazel build //...
# --config=clang --features=layering_check` reports 0 violations, because
# //include:headers exports all 79 headers to every target through
# goo_cc_library, so no include can fail. Splitting that blob is what turns the
# feature on for real.
#
# VISIBILITY needs no toolchain support and works today. See
# :visibility_violation below.


cc_library(name = "a", hdrs = ["a.h"])

cc_library(name = "b", hdrs = ["b.h"], deps = [":a"])

# The control: includes only its direct dep's header.
cc_library(name = "ok", srcs = ["ok.c"], deps = [":b"])

# The defect: reaches through :b to :a's header. Tagged manual so
# `bazel build //...` never sees it.
cc_library(name = "violation", srcs = ["violation.c"], deps = [":b"], tags = ["manual"])

# Visibility teeth. //src/types:tc_context is package-private, so this target
# must fail ANALYSIS. Tagged manual, so `bazel build //...` never expands to it
# and a red build is never the normal state.
#
# Bazel refuses this at analysis time rather than at compile time, which is the
# whole point: a boundary violation is caught before anything is built.
cc_library(
    name = "visibility_violation",
    srcs = ["ok.c"],
    tags = ["manual"],
    deps = [
        ":b",
        "//src/types:tc_context",
    ],
)
