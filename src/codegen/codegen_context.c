// The two CodeGenerator helpers that were closing cycles, extracted.
//
// WHY THESE TWO AND NOT THE OTHER TWENTY-NINE. Measured with nm on the built
// objects, 2026-08-30. src/codegen is 15 files and 77 inter-file edges, and 14
// of the files sit in ONE cycle. codegen.c is the hub: it exports 31 symbols
// that other files in the package call, and codegen_error alone has 11 callers.
//
// Moving all 31 was simulated first. It shrinks the cycle from 14 files to 11
// and frees three. That is a poor return for a 31-function move through a
// 2,203-line file, so it is NOT what this does.
//
// Two files reach into codegen.c for EXACTLY ONE SYMBOL EACH:
//
//   cfctx.c               codegen_create_block
//   runtime_integration.c codegen_error
//
// So moving these two frees both, and the cycle goes from 14 files to 12. Two
// functions for two files is the same return the tc_context extraction gave in
// src/types, and it is the whole of the cheap part.
//
// WHAT BELONGS HERE. A helper on CodeGenerator that decides nothing. If a
// function added here ever needs to walk an AST node or emit a value, it
// belongs back in codegen.c -- and putting it here would re-close a cycle,
// which the link reports rather than hides.
//
// THE REST OF THE CYCLE IS STRUCTURAL, not accidental. The eleven files that
// remain -- call, composite, error_union, expression, function, interface,
// lowlevel, monomorphize, nullable, statement, value_scope -- call each other
// because a recursive-descent code generator is mutually recursive: generating
// a statement generates an expression, which generates a statement. No
// accessor extraction reaches that, and pretending otherwise would move code
// without moving the problem.
#include "codegen.h"
#include <stdarg.h>
#include <stdio.h>

// Report a code-generation error and count it. Eleven of the fifteen files in
// this package call this, which is why it could not stay behind.
void codegen_error(CodeGenerator* codegen, Position pos, const char* format, ...) {
    if (!codegen) return;

    fprintf(stderr, "Error at %s:%d:%d: ", pos.filename ? pos.filename : "<unknown>", pos.line, pos.column);

    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    fprintf(stderr, "\n");
    codegen->error_count++;
}

#if LLVM_AVAILABLE

// Append a basic block to the function being generated. Guarded exactly as it
// was in codegen.c, where it sat inside the #if LLVM_AVAILABLE span that runs
// from line 571 to the end of the emitting code.
LLVMBasicBlockRef codegen_create_block(CodeGenerator* codegen, const char* name) {
    if (!codegen || !codegen->current_function) return NULL;

    return LLVMAppendBasicBlockInContext(codegen->context, codegen->current_function, name);
}

#endif
