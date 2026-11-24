# The golden fixtures, individually addressable so a probe target can take
# just the one it needs as data rather than the whole 597-file corpus.
exports_files(glob([
    "*.goo",
    "*.expected.txt",
    "*.exit",
    "*.stderr.txt",
    "*.env",
]))


# The script-backed probe gates (task 14) each read one or more fixtures by
# NAME from inside a shell script, so there is no per-target dependency to
# declare. Naming the whole set as data is honest about what those scripts can
# reach, and cheaper than auditing 597 files for which script opens which.
filegroup(
    name = "all_fixtures",
    srcs = glob([
        "*.goo",
        "*.txt",
    ]),
    visibility = ["//visibility:public"],
)

# The two test PACKAGES goo_test_probe drives with `goo test .`. all_fixtures
# stops at the top level; these need the directory, sidecar included.
filegroup(
    name = "test_packages",
    srcs = glob([
        "testpkg/**",
        "testfailpkg/**",
    ]),
    visibility = ["//visibility:public"],
)
