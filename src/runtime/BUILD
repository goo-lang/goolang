load("//tools:defs.bzl", "goo_cc_library")

# The 11 files in the Makefile's RUNTIME_SRCS, and only those. src/runtime/
# holds 13 .c files: io.c and far_transport.c are deliberately excluded,
# because RUNTIME_SRCS excludes them and this target must match what
# $(RUNTIME_LIB) actually contains.
goo_cc_library(
    name = "runtime",
    srcs = [
        # Private sibling header. In Make this is invisible: #include
        # "platform.h" resolves relative to the including file, so it is never
        # declared anywhere. Bazel stages only declared inputs, so it must be
        # named -- in srcs rather than hdrs, because it is private to this
        # package and not part of the library's interface.
        "platform.h",
        "arena.c",
        "channels.c",
        "concurrency.c",
        "deadlock.c",
        "defer.c",
        "platform.c",
        "runtime.c",
        "sync.c",
        "sync_shim.c",
        "testing.c",
        "time_shim.c",
    ],
    linkopts = [
        "-lm",
        "-pthread",
    ],
    visibility = ["//visibility:public"],
)
