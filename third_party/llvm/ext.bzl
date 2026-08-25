load(":llvm.bzl", "llvm_repo")

def _llvm_ext_impl(_mctx):
    llvm_repo(name = "llvm")

llvm_ext = module_extension(implementation = _llvm_ext_impl)
