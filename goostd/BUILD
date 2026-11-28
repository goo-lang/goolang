# The vendored stdlib, as data for any probe that imports a package.
#
# GOOROOT's contract is the directory CONTAINING goostd/, so the runner passes
# the workspace root and the resolver appends "goostd".
filegroup(
    name = "files",
    # VENDOR.tsv is the ship ledger: doc_claims_probe classifies a vendored
    # package as unshipped only by its row there.
    srcs = glob(["**/*.go", "**/*.goo"]) + ["VENDOR.tsv"],
    visibility = ["//visibility:public"],
)
