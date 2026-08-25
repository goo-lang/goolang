"""Locates LLVM through llvm-config and exposes its C API as @llvm//:llvm_c.

The headers are SYMLINKED into the repository rather than referenced by an
absolute -isystem path. Verified 2026-08-25: the absolute form fails with
"The include path ... references a path outside of the execution root", so
the symlink is load-bearing, not a style choice.

This rule is deliberately NOT hermetic -- it reads the host toolchain, exactly
as Makefile:50 does. What it adds is REFUSAL: the Makefile prints a warning and
builds with LLVM_AVAILABLE=0, so a build with no code generator still exits 0.

The version check is a FLOOR, not an equality, because the two environments
this must work in do not agree and neither is wrong:
  - this workstation has llvm-config 22.1.8 as the unversioned binary
  - .github/workflows/tests.yml runs ubuntu-24.04 and installs llvm-dev, which
    provides a VERSIONED binary (llvm-config-18 and similar) and no
    unversioned one
A floor keeps the property that matters -- a missing or too-old LLVM stops the
build loudly -- without pinning CI to a version its distribution does not ship.
"""

# Ubuntu 24.04's llvm-dev, which CI installs. Raise this only when the code
# actually starts using a newer C API symbol.
_DEFAULT_MIN_MAJOR = 18

# Searched in order when GOO_LLVM_CONFIG is unset. The unversioned name first,
# then newest to oldest, mirroring how Makefile:50 probes.
_CANDIDATES = [
    "llvm-config",
    "llvm-config-22",
    "llvm-config-21",
    "llvm-config-20",
    "llvm-config-19",
    "llvm-config-18",
]

def _run(rctx, args):
    res = rctx.execute(args)
    if res.return_code != 0:
        fail("llvm-config failed: {} -> {}".format(args, res.stderr))
    return res.stdout.strip()

def _find_llvm_config(rctx):
    explicit = rctx.getenv("GOO_LLVM_CONFIG", "")
    if explicit:
        found = rctx.path(explicit)
        if not found.exists:
            fail("GOO_LLVM_CONFIG points at {}, which does not exist".format(explicit))
        return found
    for name in _CANDIDATES:
        found = rctx.which(name)
        if found != None:
            return found
    fail(
        "no llvm-config on PATH. Looked for: {}. " +
        "Set GOO_LLVM_CONFIG to an explicit path, or install llvm-dev.".format(
            ", ".join(_CANDIDATES),
        ),
    )

def _major(version):
    head = version.split(".")[0]
    if not head.isdigit():
        fail("cannot read a major version out of llvm-config --version: " + version)
    return int(head)

def _llvm_repo_impl(rctx):
    cfg = _find_llvm_config(rctx)

    version = _run(rctx, [cfg, "--version"])
    floor = int(rctx.getenv("GOO_LLVM_MIN_MAJOR", str(_DEFAULT_MIN_MAJOR)))
    if _major(version) < floor:
        fail("goolang requires LLVM {} or newer, {} reports {}".format(
            floor,
            cfg,
            version,
        ))

    includedir = _run(rctx, [cfg, "--includedir"])
    libdir = _run(rctx, [cfg, "--libdir"])
    libs = _run(rctx, [cfg, "--libs", "core"]).split(" ")

    rctx.symlink(includedir, "include")
    rctx.file("VERSION", version + "\n")
    rctx.file("BUILD", '''load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "llvm_c",
    hdrs = glob(["include/**/*.h"]),
    includes = ["include"],
    linkopts = {linkopts},
    visibility = ["//visibility:public"],
)

exports_files(["VERSION"])
'''.format(linkopts = repr(["-L" + libdir] + libs)))

llvm_repo = repository_rule(
    implementation = _llvm_repo_impl,
    local = True,
    environ = [
        "GOO_LLVM_CONFIG",
        "GOO_LLVM_MIN_MAJOR",
        "PATH",
    ],
    doc = "Configures @llvm from the host llvm-config, asserting a version floor.",
)
