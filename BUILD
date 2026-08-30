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
# cross a package boundary, and every src/* directory has its own BUILD, so
# src/**/*.c belongs to //src/runtime, //src/types and the rest -- not to the
# root package. Collecting them would mean a filegroup inside each of the ten
# src packages. The seven script gates that scan or compile src/ are therefore
# NOT migrated in this pass; docs/superpowers/plans records which and why.
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

# The grammar conflict tripwire, for //tools:grammar_tripwire_test.
filegroup(
    name = "grammar_tripwire_script",
    srcs = ["scripts/grammar-tripwire.sh"],
    visibility = ["//visibility:public"],
)
