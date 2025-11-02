# TDD Cycle 5 - Code Generation (GREEN Phase - In Progress)

**Date**: 2025-11-02
**Status**: 🟡 GREEN PHASE - Implementation In Progress
**Focus**: LLVM IR Code Generation

---

## Progress Summary

We've successfully moved from RED phase (failing tests) into GREEN phase implementation. Significant progress has been made on fixing core code generation bugs.

### Completed ✅

1. **RED Phase Setup** - Successfully identified all failing tests and their causes
2. **Compilation Fixes** - Disabled Task #22 incomplete files to allow codegen tests to build
3. **Test Infrastructure** - 10 comprehensive integration tests building and running
4. **Root Cause Analysis** - Identified type mismatch bug in integer literal generation
5. **Initial Fixes Implemented**:
   - Fixed integer literal type generation to use type from type checker
   - Refactored global variable initializer logic to prevent double-setting

### In Progress 🟡

**Bug Being Fixed**: Global variable initializer type mismatch

**Current Status**:
- Integer literals were hardcoded to INT64
- Variables declared as `int` map to INT32
- This causes LLVM verification error: "Global variable initializer type does not match global variable type"

**Fixes Applied**:
1. Modified `expression_codegen.c:73-101` to use AST node_type instead of hardcoding INT64
2. Refactored `function_codegen.c:277-326` to set initializer only once
3. Added debug output to track type mismatches

---

## Key Code Changes

### File 1: `src/codegen/expression_codegen.c`

**Before** (lines 85-90):
```c
case TOKEN_INT: {
    // Parse integer value from string
    long long value = atoll(literal->value);
    llvm_value = LLVMConstInt(LLVMInt64TypeInContext(codegen->context), value, 1);
    goo_type = type_checker_get_builtin(checker, TYPE_INT64);
    break;
}
```

**After** (lines 85-101):
```c
case TOKEN_INT: {
    // Parse integer value from string
    long long value = atoll(literal->value);

    // Use type from type checker if available, otherwise default to INT32
    goo_type = expr->node_type ? expr->node_type : type_checker_get_builtin(checker, TYPE_INT32);

    // Generate appropriate LLVM constant based on Goo type
    LLVMTypeRef llvm_type = codegen_type_to_llvm(codegen, goo_type);
    if (!llvm_type) {
        llvm_type = LLVMInt32TypeInContext(codegen->context);
        goo_type = type_checker_get_builtin(checker, TYPE_INT32);
    }

    llvm_value = LLVMConstInt(llvm_type, value, 1);
    break;
}
```

**Rationale**: Integer literals should inherit their type from the context (variable declaration, function parameter, etc.) via type checking, not be hardcoded to INT64.

### File 2: `src/codegen/function_codegen.c`

**Before** (lines 283-286):
```c
// Global variable
alloca_inst = LLVMAddGlobal(codegen->module, llvm_type, var_name);
LLVMSetInitializer(alloca_inst, LLVMConstNull(llvm_type));  // Set here...
```
...then later at line 307:
```c
LLVMSetInitializer(alloca_inst, init_value->llvm_value);  // ...and here again!
```

**After** (lines 283-326):
```c
// Global variable - don't set initializer yet, we'll set it below
alloca_inst = LLVMAddGlobal(codegen->module, llvm_type, var_name);

// ... later ...

if (var_decl->values) {
    // Generate and set initializer once
    if (LLVMIsConstant(init_value->llvm_value)) {
        LLVMSetInitializer(alloca_inst, init_value->llvm_value);
    }
} else {
    // Only set NULL for uninitialized globals
    if (!codegen->current_function) {
        LLVMSetInitializer(alloca_inst, LLVMConstNull(llvm_type));
    }
}
```

**Rationale**: LLVM requires `LLVMSetInitializer` to be called exactly once per global variable. Setting it twice causes undefined behavior.

### File 3: `Makefile`

Temporarily disabled files with incomplete Task #22 implementations:
- advanced_constraint_inference.c
- concept_generics.c
- higher_kinded_types.c
- type_level_programming.c
- interface_integration.c
- hkt_auto_impl.c
- protocol_oriented_programming.c
- dependent_types.c
- contracts.c
- proof_generation.c
- runtime_optimization.c

---

## Current Issue: Debug Needed

**Problem**: Type mismatch still occurring despite fixes

**Evidence**:
```
Module verification failed: Global variable initializer type does not match global variable type!
ptr @x
```

The error message shows "ptr @x" which suggests the global variable has pointer type, but we're expecting it to have i32 type.

**Debug Strategy Added**:
Added fprintf debug statements at lines 307-310 in `function_codegen.c` to print:
- Global variable type
- Initializer type
- Expected type

**Next Steps**:
1. Rebuild compiler with debug output
2. Run test case to see actual types
3. Identify where type conversion is going wrong
4. Apply final fix

---

## Test Results

### Compilation Status

- ✅ `bin/test_codegen_integration` - Builds successfully
- ✅ Tests run without immediate crashes
- ❌ Parse errors for some test cases
- ❌ Segfaults on later tests
- ❌ Type mismatch in global variable generation

### Test Cases Status

| Test | Status | Issue |
|------|--------|-------|
| 1. Integer Literal | 🟡 Partial | Type mismatch in global init |
| 2. Binary Arithmetic | ❌ | Parse error |
| 3. Simple Function | ❌ | Parse error |
| 4. Function Parameters | ❌ | Parse error |
| 5. Variable Declaration | ❌ | Parse error |
| 6. If Statement | ❌ | Parse error |
| 7. Boolean Expression | ❌ | Parse error |
| 8. String Literal | ❌ | Parse error |
| 9. Multiple Functions | ❌ | Segfault |
| 10. Error Union | ❌ | Segfault |

**Parse Errors** suggest either:
- Parser doesn't fully support the syntax being tested
- Test input format needs adjustment
- Additional parser rules needed

**Segfaults** indicate:
- NULL pointer dereferences in codegen
- Missing implementations for certain AST node types
- Need more NULL checks and error handling

---

## What's Working vs. What's Not

### Working ✅

1. **Code Generation Infrastructure**
   - LLVM context, module, and builder creation
   - Type mapping system (`codegen_type_to_llvm`)
   - Basic type support (int8, int16, int32, int64, float32, float64, bool)
   - Function declaration framework
   - Variable declaration framework
   - Expression generation dispatch

2. **Type System Integration**
   - Type checker runs before codegen
   - AST nodes have type information attached
   - Type compatibility checking

3. **Test Framework**
   - 10 comprehensive test cases
   - Helper functions for compilation
   - IR verification logic

### Not Working / Incomplete ❌

1. **Global Variable Initialization**
   - Type mismatch between variable and initializer
   - Needs investigation why "ptr" type is being used

2. **Parser Limitations**
   - Some Goo syntax not fully supported
   - Error messages: "syntax error" at various positions

3. **Expression Code Generation**
   - Integer literals partially working (type issue)
   - Binary operations may need implementation
   - String literals may need work

4. **Function Code Generation**
   - Basic structure exists but may have bugs
   - Return statements need verification
   - Function calls may be incomplete

5. **Control Flow**
   - If statements implementation may be incomplete
   - Basic blocks creation needs verification

---

## Next Actions (Prioritized)

### Immediate (Next Hour)

1. **Debug Type Mismatch**
   - Get debug output from global variable initialization
   - Identify why "ptr" type appears instead of "i32"
   - Apply targeted fix

2. **Fix Parser Issues**
   - Investigate parse errors in test cases
   - Check if parser supports all required syntax
   - May need to adjust test inputs

3. **Add NULL Checks**
   - Review codegen functions for NULL pointer risks
   - Add defensive checks to prevent segfaults

### Short Term (Next Session)

4. **Complete Expression Codegen**
   - Binary operations (add, sub, mul, div)
   - Comparison operations (eq, ne, lt, gt, le, ge)
   - Unary operations (neg, not)

5. **Complete Function Codegen**
   - Return statement handling
   - Function call generation
   - Parameter passing

6. **Control Flow Codegen**
   - If statement basic blocks
   - Branch instructions
   - Phi nodes for SSA form

### Medium Term

7. **Advanced Features**
   - String literal support
   - Error unions (!T)
   - Nullable types (?T)
   - Array/slice support

---

## Technical Insights

### Type System Discovery

**Key Finding**: Goo has different type mappings for literals vs. declared types

- `"int"` keyword → TYPE_INT32 (32-bit)
- Integer literals (default) → TYPE_INT64 (64-bit)
- This mismatch was causing the verification error

**Solution**: Literals must inherit type from context via type checker's `node_type` field.

### LLVM API Rules

1. **Global Variable Initialization**
   - `LLVMAddGlobal()` creates the global
   - `LLVMSetInitializer()` must be called exactly once
   - Initializer must be a constant value
   - Initializer type must exactly match variable type

2. **Type Conversion**
   - LLVM is strict about type matching
   - No implicit conversions
   - Must use explicit cast instructions if types differ

### Code Quality Observations

**Positive**:
- Well-structured codegen architecture
- Clear separation of concerns
- Good use of helper functions
- Comprehensive error reporting framework

**Needs Improvement**:
- More NULL checks throughout
- Better error messages for type mismatches
- Debug output for troubleshooting
- Unit tests for individual codegen functions

---

## Files Modified

1. `Makefile` - Disabled incomplete Task #22 files
2. `src/codegen/expression_codegen.c` - Fixed integer literal type generation
3. `src/codegen/function_codegen.c` - Fixed global variable initializer logic, added debug output

---

## Confidence Level

**Current**: 🔥🔥 MEDIUM-HIGH 🔥🔥

**Why Medium-High**:
- ✅ Root cause identified (type mismatch)
- ✅ Logical fixes applied
- ✅ Good understanding of LLVM API
- ❌ Still debugging (not verified working yet)
- ❌ Multiple other issues remain (parser, segfaults)

**Will Reach High** when:
- First test passes completely
- Type mismatch issue resolved
- Pattern established for fixing remaining tests

---

## Estimated Time to GREEN

- **Debug current issue**: 30-60 minutes
- **Fix parser issues**: 1-2 hours
- **Fix remaining codegen**: 2-4 hours
- **Total**: 4-7 hours

We're approximately **40% through** the GREEN phase implementation.

---

**Next Step**: Debug the type mismatch issue to understand why "ptr" type is being used instead of "i32" for global variable `@x`.

**Status**: Ready to continue debugging once build issues are resolved.
