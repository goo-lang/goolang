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
    deps = [
        "//include:runtime_h",
    ],
)

# The archive's contents: Makefile:137 RUNTIME_OBJS, minus far_transport.c.
#
# This is NOT :runtime above. That one carries the 11 of RUNTIME_SRCS and
# exists for obj_header_test, which is the only unit suite the Makefile links
# narrowly. This one adds io.c, which is in the archive and not in
# RUNTIME_SRCS.
#
# far_transport.c is DELIBERATELY EXCLUDED, so this is 12 files and not 13.
# It includes <nng/nng.h>, and Makefile:179-180 gives it
# -I$(NNG_BUILD)/include plus a dependency on $(NNG_LIB): it cannot compile
# without NNG, let alone link. Phase 3c adds it back.
#
# Measured 2026-08-25: a 12-object archive built exactly this way passes 486
# of 495 golden fixtures, the identical split a 13-object one gives, with the
# same nine failures -- far_shim_probe and eight lanes_* probes, which are the
# only fixtures that reach the far transport.
goo_cc_library(
    name = "runtime_full",
    srcs = [
        "platform.h",
        "arena.c",
        "channels.c",
        "concurrency.c",
        "deadlock.c",
        "defer.c",
        "io.c",
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
    deps = [
        "//include:runtime_h",
    ],
)

# The runtime archive under the name the compiler expects, so it can be handed
# over with GOO_RUNTIME.
#
# A COPY, not a rebuild. The plan called for an explicit `ar -D rcs` to
# guarantee determinism, and that was wrong on both counts. Bazel's own archive
# is already deterministic -- it links with ZERO_AR_DATE=1, and every member
# reads uid 0, gid 0, epoch 0, which is exactly what archive-determinism-probe
# asserts. And `ar rcs out.a in.a` would have added the input archive as a
# MEMBER rather than extracting its objects, producing a nested archive that
# links nothing.
#
# The Makefile's recipe additionally does `addlib` on libnng.a. That is phase
# 3c, and it is the hard half: addlib copies each member's header VERBATIM, so
# determinism cannot be imposed at this level and has to be achieved in NNG's
# own build. That is why the Makefile passes three CMAKE_C_ARCHIVE_* overrides.
genrule(
    name = "goo_runtime_archive",
    srcs = [":runtime_full"],
    outs = ["libgoo_runtime.a"],
    cmd = "for f in $(SRCS); do case $$f in *.a) cp $$f $@ ;; esac; done",
    visibility = ["//visibility:public"],
)
