"""Locates LLVM through the host llvm-config and exposes its C API.

The headers are SYMLINKED into the repository rather than referenced by an
absolute -isystem path. Verified 2026-08-25: the absolute form fails with
"The include path ... references a path outside of the execution root", so
the symlink is load-bearing, not a style choice.

This rule is deliberately NOT hermetic -- it reads the host toolchain, exactly
as Makefile:50 does. What it adds is refusal: the Makefile prints a warning and
builds with LLVM_AVAILABLE=0, so a build with no code generator still exits 0.
"""

_REQUIRED_MAJOR = "22."

def _run(rctx, args):
    res = rctx.execute(args)
    if res.return_code != 0:
        fail("llvm-config failed: {} -> {}".format(args, res.stderr))
    return res.stdout.strip()

def _llvm_repo_impl(rctx):
    cfg = rctx.which("llvm-config")
    if cfg == None:
        fail("llvm-config is not on PATH. goolang requires LLVM 22.")

    version = _run(rctx, [cfg, "--version"])
    if not version.startswith(_REQUIRED_MAJOR):
        fail("goolang requires LLVM {}x, llvm-config reports {}".format(
            _REQUIRED_MAJOR,
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
    doc = "Configures @llvm from the host llvm-config, asserting the version.",
)
