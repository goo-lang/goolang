load("@rules_cc//cc:defs.bzl", "cc_library")
load("//tools:defs.bzl", "goo_cc_library")

# src/runtime is 12 files and 21 inter-file edges. Measured with nm on the built
# objects, 2026-08-30: only FIVE of them are in a cycle. The other seven were
# acyclic all along, and the two coarse targets below were the only thing
# hiding it. Nothing in this commit changes a line of C.
#
#   cycle    concurrency, deadlock, platform, runtime, sync
#   acyclic  arena, channels, defer, io, sync_shim, testing, time_shim
#
# The edges, so the deps below are checkable rather than trusted:
#
#   arena      -> core                goo_panic, goo_zerobase
#   channels   -> core                the sync, concurrency, runtime and
#                                     platform families
#   defer      -> core                goo_free, goo_realloc
#   sync_shim  -> core                the goo_mutex and goo_waitgroup families
#   testing    -> core                goo_exit
#   time_shim  -> channels, core      goo_chan_*, goo_platform_*, goo_go
#   io         -> nothing at all      a true leaf
#
# ONE LIST, used by both the aggregate target and the archive genrule below.
# Two lists would drift, and a runtime archive missing an object fails at the
# link of every compiled Goo program rather than here.
RUNTIME_UNITS = [
    ":arena",
    ":channels",
    ":core",
    ":defer",
    ":sync_shim",
    ":testing",
    ":time_shim",
]

# Private sibling header. In Make this is invisible: #include "platform.h"
# resolves relative to the including file, so it is never declared anywhere.
# Bazel stages only declared inputs, so it is a target -- six files include it.
cc_library(
    name = "platform_h",
    hdrs = ["platform.h"],
)

# The five files that genuinely call each other. runtime -> deadlock ->
# concurrency -> runtime is the loop, with sync and platform joined to it.
#
# I MEASURED THE CUT AND DID NOT TAKE IT. Exactly six symbols from runtime.c
# are reached by the rest of the package -- goo_alloc, goo_free, goo_realloc,
# goo_panic, goo_zerobase and goo_exit. Extracting them would cut
# concurrency -> runtime, sync -> runtime and platform -> runtime, leaving a
# three-file cycle and freeing runtime.c and platform.c.
#
# It is not clean. goo_alloc and goo_free are written against the ARC object
# header, whose two static inline helpers -- goo_obj_headerless and
# goo_obj_header -- are used at ten more sites in runtime.c by goo_retain,
# goo_release and goo_make_immortal, which would stay. Extracting the
# allocator therefore means extracting reference counting with it: about 320
# lines, and the most safety-critical code in the project.
#
# That is a defensible refactor -- "the ARC core" is a real unit -- but it is
# not a build-graph change, and it needs the ARC measurement harness
# (docs/adr/0005-measurements) rather than the golden suite alone. Do not
# start it as a side effect of tidying targets.
goo_cc_library(
    name = "core",
    srcs = [
        "concurrency.c",
        "deadlock.c",
        "platform.c",
        "runtime.c",
        "sync.c",
    ],
    linkopts = [
        "-lm",
        "-pthread",
    ],
    deps = [
        ":platform_h",
        "//include:runtime_h",
    ],
)

goo_cc_library(
    name = "arena",
    srcs = ["arena.c"],
    deps = [
        ":core",
        "//include:runtime_h",
    ],
)

goo_cc_library(
    name = "channels",
    srcs = ["channels.c"],
    deps = [
        ":core",
        ":platform_h",
        "//include:runtime_h",
    ],
)

goo_cc_library(
    name = "defer",
    srcs = ["defer.c"],
    deps = [
        ":core",
        "//include:runtime_h",
    ],
)

goo_cc_library(
    name = "sync_shim",
    srcs = ["sync_shim.c"],
    deps = [
        ":core",
        ":platform_h",
        "//include:runtime_h",
    ],
)

goo_cc_library(
    name = "testing",
    srcs = ["testing.c"],
    deps = [
        ":core",
        "//include:runtime_h",
    ],
)

goo_cc_library(
    name = "time_shim",
    srcs = ["time_shim.c"],
    deps = [
        ":channels",
        ":core",
        ":platform_h",
        "//include:runtime_h",
    ],
)

# io.c is in the archive and NOT in the Makefile's RUNTIME_SRCS, which is why
# it sits outside RUNTIME_UNITS and is named separately by the genrule.
goo_cc_library(
    name = "io",
    srcs = ["io.c"],
    deps = ["//include:runtime_h"],
)

# The 11 files of the Makefile's RUNTIME_SRCS, as an aggregate. io.c and
# far_transport.c are excluded, because RUNTIME_SRCS excludes them and this
# target must match what $(RUNTIME_LIB) contains. Kept for
# //tests/unit/runtime:obj_header_test, its only consumer.
cc_library(
    name = "runtime",
    visibility = ["//tests/unit/runtime:__pkg__"],
    deps = RUNTIME_UNITS,
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
# far_transport.c is DELIBERATELY EXCLUDED from RUNTIME_UNITS and from :io, so
# the archive is 12 objects and not 13. It includes <nng/nng.h>, and
# Makefile:179-180 gives it -I$(NNG_BUILD)/include plus a dependency on
# $(NNG_LIB): it cannot compile without NNG, let alone link. Phase 3c adds it
# back.
#
# Measured 2026-08-25: a 12-object archive built exactly this way passes 486 of
# 495 golden fixtures, the identical split a 13-object one gives, with the same
# nine failures -- far_shim_probe and eight lanes_* probes, which are the only
# fixtures that reach the far transport.
#
# :runtime_full is GONE. It was an aggregate with no consumers once the genrule
# started naming RUNTIME_UNITS directly -- `bazel query rdeps(//..., it)`
# returned nothing, against a positive control on :runtime that returned
# obj_header_test.
genrule(
    name = "goo_runtime_archive",
    srcs = RUNTIME_UNITS + [":io"],
    outs = ["libgoo_runtime.a"],
    # A MERGE, not a copy. It used to copy one 12-object archive, which stopped
    # working the moment the package was split: an aggregate cc_library has no
    # srcs of its own, so its own .a is empty and the copy would have produced
    # an archive that links nothing.
    #
    # `ar rcs out.a in.a` is still wrong for the reason the old comment gave --
    # it adds the input archive as a MEMBER and nests it. So each input is
    # EXTRACTED first, then one archive is built from the objects.
    #
    # -D is deterministic mode: zero uid, gid and mtime on every member. Bazel's
    # own archives already have that, and archive-determinism-probe asserts it,
    # so stating it here keeps the property true of the merge as well as of the
    # inputs. The $$tmp/*.o glob sorts, so member ORDER is deterministic too.
    cmd = """
root=$$PWD
tmp=$$(mktemp -d)
for f in $(SRCS); do
  case $$f in *.a) ( cd $$tmp && ar x $$root/$$f ) ;; esac
done
ar rcsD $$root/$@ $$tmp/*.o
rm -rf $$tmp
""",
    visibility = ["//visibility:public"],
)
