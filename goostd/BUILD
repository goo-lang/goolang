# The vendored stdlib, as data for any probe that imports a package.
#
# GOOROOT's contract is the directory CONTAINING goostd/, so the runner passes
# the workspace root and the resolver appends "goostd".
filegroup(
    name = "files",
    srcs = glob(["**/*.go", "**/*.goo"]),
    visibility = ["//visibility:public"],
)
