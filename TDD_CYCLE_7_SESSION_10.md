# TDD Cycle 7 - Session 10: Method Receiver Implementation (Work in Progress)

**Date**: 2025-11-03
**Status**: 🟡 80% Stable (8/10 tests passing)
**Pass Rate**: 80% - Method receiver codegen partially implemented

---

## Session Overview

Continued TDD Cycle 7 (Structs & Methods) from 80%, focusing on implementing method receivers to reach 100%. Made significant progress on codegen infrastructure but method calls still need debugging.

---

## Implementation Work

### Method Receiver Codegen

**File**: `src/codegen/function_codegen.c`

#### Change 1: Function Signature with Receiver (Lines 56-98)

Added support for including receiver as first parameter in LLVM function signature:

```c
// Check if this is a method (has receiver)
int has_receiver = (func_decl->receiver_name && func_decl->receiver_type);
Type* receiver_type = NULL;
if (has_receiver) {
    receiver_type = type_from_ast(checker, func_decl->receiver_type);
    llvm_param_count = 1; // Receiver is first parameter
}

// Allocate LLVM parameter types array (receiver + regular params)
if (llvm_param_count > 0) {
    param_types = malloc(sizeof(LLVMTypeRef) * llvm_param_count);
    int idx = 0;

    // Add receiver type first if this is a method
    if (has_receiver && receiver_type) {
        param_types[idx++] = codegen_type_to_llvm(codegen, receiver_type);
    }

    // Add regular parameters
    for (int i = 0; i < param_count; i++) {
        param_types[idx++] = codegen_type_to_llvm(codegen, func_type_info->data.function.param_types[i]);
    }
}
```

#### Change 2: Method Name Mangling (Lines 115-140)

Implemented name mangling for methods to avoid conflicts:

```c
// Use mangled name for methods
const char* func_name = func_decl->name;
char* mangled_name = NULL;
if (has_receiver && receiver_type) {
    const char* type_name = NULL;
    if (receiver_type->kind == TYPE_STRUCT) {
        type_name = receiver_type->data.struct_type.name;
    } else if (receiver_type->kind == TYPE_POINTER &&
               receiver_type->data.pointer.pointee_type &&
               receiver_type->data.pointer.pointee_type->kind == TYPE_STRUCT) {
        type_name = receiver_type->data.pointer.pointee_type->data.struct_type.name;
    }

    if (type_name) {
        size_t len = strlen(type_name) + strlen(func_decl->name) + 2;
        mangled_name = malloc(len);
        snprintf(mangled_name, len, "%s_%s", type_name, func_decl->name);
        func_name = mangled_name;
    }
}

function_type = LLVMFunctionType(llvm_return_type, param_types, llvm_param_count, 0);
function = LLVMAddFunction(codegen->module, func_name, function_type);
```

**Mangling Scheme**: `TypeName_methodName`
- Example: `Counter_get_value` for `func (c Counter) get_value()`
- Supports both value receivers (`Counter`) and pointer receivers (`*Counter`)

#### Change 3: Receiver Parameter Handling (Lines 133-151)

Added receiver to function's local scope:

```c
// Handle receiver for methods (receiver is the first implicit parameter)
int llvm_param_index = 0;
if (func_decl->receiver_name && func_decl->receiver_type) {
    Type* receiver_type = type_from_ast(checker, func_decl->receiver_type);
    if (receiver_type) {
        LLVMValueRef receiver_param = LLVMGetParam(function, llvm_param_index++);
        LLVMTypeRef receiver_llvm_type = codegen_type_to_llvm(codegen, receiver_type);

        // Create alloca for receiver
        LLVMValueRef receiver_alloca = codegen_create_entry_alloca(codegen, receiver_llvm_type, func_decl->receiver_name);
        LLVMBuildStore(codegen->builder, receiver_param, receiver_alloca);

        // Add to value table
        ValueInfo* receiver_info = value_info_new(func_decl->receiver_name, receiver_alloca, receiver_type);
        receiver_info->is_lvalue = 1;
        receiver_info->is_initialized = 1;
        codegen_add_value(codegen, receiver_info);
    }
}
```

#### Change 4: Updated Parameter Index Handling (Lines 164-197)

Modified parameter handling to use separate index for LLVM parameters:

```c
// Use llvm_param_index for getting LLVM parameters (accounts for receiver)
LLVMValueRef param_value = LLVMGetParam(function, llvm_param_index++);
```

---

## Current Status

### Passing Tests (8/10) ✅

1-8: All struct tests passing (declaration, fields, literals, parameters, return, nested, multiple)

### Failing Tests (2/10) ❌

9. **test_method_declaration**
   ```goo
   func (c Counter) get_value() int {
       return c.value;  // Error: Undefined identifier 'c'
   }
   ```
   - Error: "Undefined identifier 'c'" at line 6:20
   - Also: "Cannot access field 'value' on type *?"
   - Receiver not accessible in method body

10. **test_pointer_receiver_method**
    ```goo
    func (c *Counter) increment() {
        c.value = c.value + 1;
    }
    ```
    - Similar undefined identifier error
    - Pointer receiver not accessible

---

## Problem Analysis

### What's Working ✅

1. **Parser**: Correctly parses method syntax `func (receiver Type) method()`
2. **Type Checker**: Adds receiver to scope during type checking (lines 381-390 of type_checker.c)
3. **Function Signature**: LLVM function created with receiver as first parameter
4. **Name Mangling**: Methods get unique names (`TypeName_methodName`)

### What's Broken ❌

1. **Receiver Access**: Inside method body, receiver identifier not found
2. **Field Access**: Cannot access fields on receiver
3. **Method Calls**: Selector expressions (`obj.method()`) not generating correct calls

### Root Cause Hypothesis

The receiver is being added to the codegen value table, but something is clearing it or the lookup isn't finding it. Possible issues:

1. **Scope Issue**: Value table might be cleared before method body codegen
2. **Lookup Issue**: `codegen_lookup_value()` not finding the receiver
3. **Method Call Issue**: The real problem might be in how `c.get_value()` is compiled (selector expression)

---

## Next Steps for Debugging

### Priority 1: Verify Receiver in Value Table
- Add debug logging to confirm receiver is added
- Check if value table is cleared before body generation
- Verify `codegen_add_value()` is being called

### Priority 2: Check Method Call Codegen
- Investigate `codegen_generate_selector_expr()`
- Methods are called via selector: `object.method()`
- Need to generate call with object as first parameter

### Priority 3: Test Receiver Lookup
- Create minimal test case
- Add verbose error output
- Trace through value table lookup

---

## Type Checker Context

The type checker (already working correctly) handles receivers like this:

```c
// From src/types/type_checker.c:381-390
// Add receiver to scope if this is a method
if (func->receiver_name && func->receiver_type) {
    Type* receiver_type = type_from_ast(checker, func->receiver_type);
    if (receiver_type) {
        Variable* receiver_var = variable_new(func->receiver_name, receiver_type, func->base.pos);
        if (receiver_var) {
            receiver_var->is_initialized = 1;
            scope_add_variable(checker->current_scope, receiver_var);
        }
    }
}
```

This works fine - type checking succeeds. The issue is purely in codegen.

---

## Code Changes Summary

**File Modified**: `src/codegen/function_codegen.c`

**Lines Changed**:
- 56-98: Parameter type array building with receiver
- 115-140: Method name mangling
- 133-151: Receiver parameter handling
- 164-197: Updated parameter indexing

**Total**: ~80 lines of new/modified code

---

## Time Investment

**Session 10**: ~2 hours (Implementation + debugging)
**Status**: Infrastructure in place, debugging needed

---

## Recommendation

**Continue Debugging**: We're close to a solution. The infrastructure is mostly correct:
- LLVM function signatures include receiver ✅
- Name mangling works ✅
- Parameter setup logic is sound ✅

**Issue is likely**:
- Method call codegen (selector expressions)
- OR value table lookup timing

**Estimated Time to Fix**: 1-2 hours of focused debugging

---

**Session 10 Summary**: Substantial progress on method receiver codegen infrastructure. Receiver parameter handling implemented but method calls/receiver access still need debugging.
