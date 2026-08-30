# parity.sh reads the Makefile to enumerate the gate net, so the Makefile must
# be an exported file that an sh_test can take as data.
exports_files(["Makefile"])

# The script-backed probe gates (task 14). scripts/ is deliberately NOT its own
# package: //tools:grammar_tripwire_test already reaches scripts/ through the
# filegroup below, and a scripts/BUILD would put those files in a subpackage
# the parent can no longer name.
exports_files(glob(["scripts/*.sh"]))

# The grammar conflict tripwire, for //tools:grammar_tripwire_test.
filegroup(
    name = "grammar_tripwire_script",
    srcs = ["scripts/grammar-tripwire.sh"],
    visibility = ["//visibility:public"],
)
