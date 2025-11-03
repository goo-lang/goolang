# TDD Cycle 12 Complete: Slices ✅

**Status:** GREEN (10/10 tests passing - 100%)
**Date:** 2025-11-03
**Commit:** 8f89774

## Overview

Successfully implemented dynamic slice support in the Goo compiler, including all core operations: `make()`, `len()`, `cap()`, `append()`, indexing, and slice manipulation.

## Implementation Summary

### What Was Built

1. **Slice Type Representation**
   - Fixed slice struct definition: `{T*, i64, i64}` (pointer, length, capacity)
   - Critical bug fix: Changed from incorrect 2-field to correct 3-field layout

2. **Built-in Functions**
   - `make([]T, len)` - Create slice with specified length (capacity = length)
   - `make([]T, len, cap)` - Create slice with specified length and capacity
   - `len(slice)` - Get current length
   - `cap(slice)` - Get current capacity
   - `append(slice, elem)` - Append elements with automatic growth

3. **Slice Operations**
   - Indexing: `s[i]` for reading and writing
   - Pass slices as function parameters
   - Return slices from functions
   - Handle nil slices correctly

4. **Memory Management**
   - Dynamic allocation via `malloc()` for backing arrays
   - 2x growth strategy in `append()` when capacity exceeded
   - Zero-capacity handling (grows to capacity 1)

## Technical Details

### Critical Bug Fix

**Problem:** Segmentation fault in `LLVMBuildExtractValue()` when accessing capacity field

**Root Cause:** Slice type was defined as `{ptr, len}` (2 fields) in `type_mapping.c`, but codegen was building/accessing `{ptr, len, cap}` (3 fields)

**Solution:** Updated `TYPE_SLICE` case in `codegen_type_to_llvm()`:
```c
// Before (INCORRECT - caused segfault)
return LLVMStructTypeInContext(codegen->context,
    (LLVMTypeRef[]){
        LLVMPointerType(element_type, 0),
        LLVMInt64TypeInContext(codegen->context)
    }, 2, 0);  // Only 2 fields!

// After (CORRECT)
return LLVMStructTypeInContext(codegen->context,
    (LLVMTypeRef[]){
        LLVMPointerType(element_type, 0),  // ptr
        LLVMInt64TypeInContext(codegen->context),  // len
        LLVMInt64TypeInContext(codegen->context)   // cap
    }, 3, 0);  // All 3 fields present
```

### Parser Changes

Allowed type nodes as primary expressions to support `make([]int, 5)` syntax:
```yacc
primary_expr:
    | slice_type { $ = $1; }  /* Allow slice types in expression context */
    | map_type { $ = $1; }
    | chan_type { $ = $1; }
```

### Type Checker Enhancements

Added builtin function type checking:
- Recognize `make`, `len`, `cap`, `append` as builtins
- Validate type arguments in `make()`
- Check argument types for `cap()` (must be slice or channel)
- Verify element type compatibility in `append()`

### Code Generation

**make() Implementation:**
```c
// Allocate backing array: ptr = malloc(cap * sizeof(element))
LLVMValueRef alloc_size = LLVMBuildMul(builder, cap, element_size, "alloc_size");
LLVMValueRef ptr = LLVMBuildCall(builder, malloc_func, &alloc_size_64, 1, "slice_data");

// Build slice struct
LLVMValueRef slice = LLVMGetUndef(slice_type);
slice = LLVMBuildInsertValue(builder, slice, ptr, 0, "slice.ptr");
slice = LLVMBuildInsertValue(builder, slice, len, 1, "slice.len");
slice = LLVMBuildInsertValue(builder, slice, cap, 2, "slice.cap");
```

**append() Implementation with Growth:**
```c
// Check if growth needed: len == cap
LLVMValueRef need_grow = LLVMBuildICmp(builder, LLVMIntEQ, len, cap, "need_grow");
LLVMBuildCondBr(builder, need_grow, grow_block, no_grow_block);

// Grow block: allocate 2x capacity
LLVMValueRef new_cap = LLVMBuildMul(builder, cap, ConstInt(2), "new_cap");
// ... allocate new array, copy old elements ...

// Merge with PHI nodes for ptr and cap
```

## Test Results

All 10 tests passing:

1. ✅ **test_slice_make** - Basic slice creation
2. ✅ **test_slice_indexing_read** - Read elements via `s[i]`
3. ✅ **test_slice_indexing_write** - Write elements via `s[i] = value`
4. ✅ **test_slice_len** - Get length with `len(s)`
5. ✅ **test_slice_cap** - Get capacity with `cap(s)`
6. ✅ **test_slice_append** - Append single element
7. ✅ **test_slice_nil** - Handle nil slices
8. ✅ **test_slice_as_parameter** - Pass slices to functions
9. ✅ **test_slice_as_return** - Return slices from functions
10. ✅ **test_multiple_appends** - Multiple appends with growth

## Files Modified

- `src/codegen/type_mapping.c` - **CRITICAL FIX:** Added capacity field to slice struct
- `src/codegen/expression_codegen.c` - Implemented make/cap/append + NULL checks
- `src/types/expression_checker.c` - Type checking for builtin functions (from prev commit)
- `src/parser/parser.y` - Allow types as expressions (from prev commit)
- `tests/unit/codegen/slice_test.c` - Complete test suite (from prev commit)
- `Makefile` - Added test-slice target (from prev commit)
- `include/codegen.h` - Function declarations (from prev commit)

## Overall TDD Progress

### Individual Cycle Results

| Cycle | Feature | Tests | Status |
|-------|---------|-------|--------|
| 7 & 8 | Loops & Arrays | 10/10 | ✅ 100% |
| 9 & 10 | Structs & Methods | 3/10 | ⚠️ 30% |
| 11.x | Multiple Returns | 9/10 | ⚠️ 90% |
| - | Switch Statements | 10/10 | ✅ 100% |
| 11 | Defer Statements | 8/8 | ✅ 100% |
| **12** | **Slices** | **10/10** | **✅ 100%** |

### Totals

**Overall:** 50/58 tests passing (86.2%)

**Fully Complete Cycles:**
- Loops & Arrays (10/10)
- Switch Statements (10/10)
- Defer Statements (8/8)
- **Slices (10/10)** ← NEW!

**Needs Work:**
- Structs & Methods: 7 failures - likely related to method calls or struct literals
- Multiple Returns: 1 failure - edge case in multi-return handling

## Next Steps

### Option 1: Fix Failing Tests (Recommended)
- Debug 7 struct/method failures to boost from 30% → 100%
- Fix 1 multiple return failure
- Would bring overall TDD to 56/58 (96.6%)

### Option 2: New TDD Cycle - Maps
- Implement `map[K]V` types
- Built-ins: `make(map[K]V)`, `delete(m, key)`, `len(m)`
- Map literals, indexing, iteration
- ~10 tests

### Option 3: New TDD Cycle - Interfaces
- Interface types and method sets
- Type assertions `v.(T)`
- Type switches
- Interface satisfaction checking
- ~12 tests

## Key Learnings

1. **Type Layout Matters:** LLVM is strict about struct layout. The number of fields in the type definition MUST match what you build/extract.

2. **Defensive Programming:** NULL checks helped identify the bug location quickly during debugging.

3. **Parser Flexibility:** Allowing types in expression contexts (for builtin type arguments) was simpler than creating special grammar rules.

4. **Growth Strategies:** The 2x doubling strategy is standard for dynamic arrays, with special handling for zero capacity.

## Debugging Journey

1. **Initial symptom:** Segfault immediately on test execution
2. **GDB investigation:** Crash in `LLVMBuildExtractValue()` at line 1788
3. **Added NULL checks:** Values passed checks, still crashed
4. **Inspected LLVM code:** Crash in `llvm::Value::setNameImpl()`
5. **Checked type definition:** Found slice had only 2 fields, not 3!
6. **Fixed type layout:** Changed from `{ptr, len}` to `{ptr, len, cap}`
7. **Result:** All tests immediately passed!

The debugging process took about 30 minutes, with the fix being a single-line change that had enormous impact.

## Conclusion

TDD Cycle 12 is complete with all tests passing. Slices are now fully functional in the Goo compiler, supporting all core operations. The critical bug fix in the type definition ensures that slice operations work correctly at runtime.

The overall TDD progress is at 86.2%, with 4 cycles at 100% completion. The next priority should be fixing the failing struct/method and multiple return tests to push overall completion above 95%.
