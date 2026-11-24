# parity.sh reads the Makefile to enumerate the gate net, so the Makefile must
# be an exported file that an sh_test can take as data.
exports_files(["Makefile"])

# The script-backed probe gates (task 14). scripts/ is deliberately NOT its own
# package: //tools:grammar_tripwire_test already reaches scripts/ through the
# filegroup below, and a scripts/BUILD would put those files in a subpackage
# the parent can no longer name.
exports_files(glob(["scripts/*.sh"]))

# Task 14 needs whole-directory reads as sh_test data.
#
# THERE IS NO //:c_sources, and there cannot be one from here. A glob does not
# cross a package boundary, so src/**/*.c mostly belongs to //src/runtime,
# //src/types and the other nine src/* packages, each of which carries its own
# probe_sources filegroup (task 14e). NOT EVERY src/* DIRECTORY HAS A BUILD,
# though: src/ide does not, so its one file is an ORPHAN no src/* filegroup
# reaches. See src_orphan_sources below.
#
# scripts/ and .github/workflows/ have no BUILD of their own, so these two
# globs do resolve.
filegroup(
    name = "probe_scripts",
    srcs = glob(["scripts/*.sh"]) + glob(["scripts/*.txt"], allow_empty = True),
    visibility = ["//visibility:public"],
)

filegroup(
    name = "workflow_files",
    srcs = glob([".github/workflows/*.yml"]),
    visibility = ["//visibility:public"],
)

# A glob never crosses a package boundary, so from the root this collects
# exactly the files that no src/* package owns -- today src/ide/lsp_enhanced.c,
# part of a framework quarantined to attic/src (CLAUDE.md) that kept its lone
# source file behind with no BUILD of its own. Stays correct if another
# orphan directory appears: a source file dropped under a NEW, BUILD-less
# src/* directory lands here too, rather than silently going unscanned.
filegroup(
    name = "src_orphan_sources",
    srcs = glob(["src/**/*.c", "src/**/*.h"], allow_empty = True),
    visibility = ["//tests/probes:__pkg__"],
)

# The grammar conflict tripwire, for //tools:grammar_tripwire_test.
filegroup(
    name = "grammar_tripwire_script",
    srcs = ["scripts/grammar-tripwire.sh"],
    visibility = ["//visibility:public"],
)
