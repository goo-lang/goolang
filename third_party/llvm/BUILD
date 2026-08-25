load("@rules_cc//cc:defs.bzl", "cc_test")

cc_test(
    name = "llvm_smoke",
    size = "small",
    srcs = ["llvm_smoke.c"],
    deps = ["@llvm//:llvm_c"],
)
