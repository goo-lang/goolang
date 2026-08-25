# parity.sh reads the Makefile to enumerate the gate net, so the Makefile must
# be an exported file that an sh_test can take as data.
exports_files(["Makefile"])

# The grammar conflict tripwire, for //tools:grammar_tripwire_test.
filegroup(
    name = "grammar_tripwire_script",
    srcs = ["scripts/grammar-tripwire.sh"],
    visibility = ["//visibility:public"],
)
