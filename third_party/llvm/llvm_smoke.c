// Proves @llvm//:llvm_c exposes a usable LLVM C API: headers resolve, the
// symbols link, and a context round-trips. Deliberately tiny -- this gates the
// dependency, not LLVM itself.
#include <llvm-c/Core.h>

int main(void) {
    LLVMContextRef ctx = LLVMContextCreate();
    LLVMModuleRef mod = LLVMModuleCreateWithNameInContext("smoke", ctx);
    LLVMDisposeModule(mod);
    LLVMContextDispose(ctx);
    return 0;
}
