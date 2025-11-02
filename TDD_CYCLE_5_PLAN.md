# TDD Cycle 5: Code Generation - PLAN

**Date**: 2025-11-02
**Status**: 🚧 PLANNING
**Previous**: Cycle 4 achieved 100% type checker coverage

---

## Objective

Implement and test LLVM IR code generation for basic Goo programs using TDD methodology.

---

## Current Status

### What We Have ✅

- ✅ **Parser**: 100% working - converts source to AST
- ✅ **Type Checker**: 100% working - validates semantics
- 🚧 **Code Generator**: Partial implementation exists, needs testing

### Compiler Pipeline

```
Source Code (.goo)
    ↓
Lexer (Tokenization)          ✅ 100% Working
    ↓
Parser (AST Generation)       ✅ 100% Working (22 tests)
    ↓
Type Checker (Semantics)      ✅ 100% Working (10 tests)
    ↓
Code Generator (LLVM IR)      🎯 NEXT: TDD Cycle 5
    ↓
Runtime System                🚧 Partial
    ↓
Executable Binary
```

---

## TDD Cycle 5 Goals

### Primary Goal
**Test and validate LLVM IR code generation for basic Goo programs**

### Success Criteria
1. ✅ Create 10-15 code generation tests
2. ✅ Achieve 80%+ test pass rate
3. ✅ Generate valid LLVM IR for basic programs
4. ✅ Verify correctness via LLVM tools

---

## Test Plan

### Test Categories

#### 1. Basic Expression Codegen (5 tests)
- Integer literals → LLVM constants
- Binary arithmetic (+, -, *, /)
- Variable declarations and loads
- Simple assignments
- Boolean expressions

#### 2. Function Codegen (3 tests)
- Function definition with parameters
- Function calls
- Return statements

#### 3. Control Flow (3 tests)
- If statements
- If-else statements
- Simple loops

#### 4. Type System Integration (2 tests)
- Error unions in LLVM IR
- Nullable types in LLVM IR

#### 5. End-to-End (2 tests)
- Complete program compilation
- LLVM IR validation (llvm-as)

---

## Example Tests to Write

### Test 1: Integer Literal Codegen
```c
void test_codegen_integer_literal() {
    // Given: Source code with integer literal
    const char* source = "package main\nvar x int = 42\n";
    
    // When: Generate LLVM IR
    char* ir = compile_to_llvm_ir(source);
    
    // Then: IR contains constant 42
    ASSERT_CONTAINS(ir, "i32 42");
}
```

### Test 2: Function Codegen
```c
void test_codegen_simple_function() {
    // Given: Function definition
    const char* source = 
        "package main\n"
        "func add(a int, b int) int {\n"
        "    return a + b\n"
        "}\n";
    
    // When: Generate LLVM IR
    char* ir = compile_to_llvm_ir(source);
    
    // Then: IR contains function definition
    ASSERT_CONTAINS(ir, "define i32 @add(i32 %a, i32 %b)");
    ASSERT_CONTAINS(ir, "add i32");
    ASSERT_CONTAINS(ir, "ret i32");
}
```

### Test 3: Error Union Codegen
```c
void test_codegen_error_union() {
    // Given: Function with error union return
    const char* source = 
        "package main\n"
        "func divide(a int, b int) !int {\n"
        "    if b == 0 {\n"
        "        return error(\"division by zero\")\n"
        "    }\n"
        "    return a / b\n"
        "}\n";
    
    // When: Generate LLVM IR
    char* ir = compile_to_llvm_ir(source);
    
    // Then: IR contains error union structure
    ASSERT_CONTAINS(ir, "%error_union");
    ASSERT_CONTAINS(ir, "i1"); // error flag
}
```

---

## Implementation Strategy

### Phase 1: RED (Create Failing Tests)
1. Create `tests/unit/codegen/codegen_test.c`
2. Write 10-15 comprehensive tests
3. Tests will fail initially (no/incomplete codegen)

### Phase 2: GREEN (Make Tests Pass)
1. Fix expression codegen (literals, variables, binary ops)
2. Fix function codegen (declarations, calls, returns)
3. Fix control flow (if statements)
4. Integrate with type system
5. Achieve 80%+ pass rate

### Phase 3: REFACTOR (Clean Up)
1. Remove debug code
2. Improve error messages
3. Optimize IR generation
4. Document remaining limitations

---

## Technical Considerations

### LLVM Integration
- Use existing LLVM infrastructure (already in Makefile)
- Generate valid LLVM IR text format
- Validate with `llvm-as` (LLVM assembler)

### Error Union Representation
```llvm
; Error union !int represented as:
%error_union.int = type { i1, i32 }
; i1 = is_error flag
; i32 = value (or error code)
```

### Nullable Type Representation
```llvm
; Nullable ?int represented as:
%nullable.int = type { i1, i32 }
; i1 = is_null flag
; i32 = value
```

### Function Signatures
```llvm
; Goo: func add(a int, b int) int
define i32 @add(i32 %a, i32 %b) {
  %result = add i32 %a, %b
  ret i32 %result
}
```

---

## Files to Create/Modify

### New Files
1. `tests/unit/codegen/codegen_test.c` - Code generation tests
2. `tests/unit/codegen/codegen_test_framework.h` - Test helpers

### Modified Files
1. `src/codegen/expression_codegen.c` - Fix expression codegen
2. `src/codegen/function_codegen.c` - Fix function codegen
3. `src/codegen/runtime_integration.c` - Fix runtime integration
4. `Makefile` - Add codegen test target

---

## Expected Challenges

### 1. LLVM IR Syntax
**Challenge**: Getting LLVM IR syntax exactly right
**Solution**: Use LLVM documentation, validate with llvm-as

### 2. Error Union Codegen
**Challenge**: Representing error unions in LLVM
**Solution**: Use struct type with error flag + value

### 3. Type System Integration
**Challenge**: Converting Goo types to LLVM types
**Solution**: Build type mapping in codegen

### 4. Runtime Integration
**Challenge**: Calling runtime functions (error(), print(), etc.)
**Solution**: Define runtime function declarations in IR

---

## Success Metrics

### Must Have (Required for Success)
- ✅ 10+ codegen tests created
- ✅ 80%+ test pass rate
- ✅ Valid LLVM IR generated
- ✅ Basic expressions work
- ✅ Simple functions work

### Nice to Have (Bonus)
- ✅ 100% test pass rate
- ✅ Error unions fully working
- ✅ Nullable types fully working
- ✅ Complete control flow
- ✅ End-to-end compilation

---

## Timeline Estimate

- **RED Phase**: 1-2 hours (write tests)
- **GREEN Phase**: 3-4 hours (implement codegen)
- **REFACTOR Phase**: 1 hour (cleanup)
- **Total**: 5-7 hours

---

## Next Steps

1. ✅ Create test file structure
2. ✅ Write first batch of tests (RED phase)
3. ✅ Run tests (expect failures)
4. ✅ Implement codegen fixes (GREEN phase)
5. ✅ Achieve 80%+ pass rate
6. ✅ Document results (TDD_CYCLE_5_COMPLETE.md)

---

## Questions to Answer

- [ ] What LLVM IR features do we need?
- [ ] How to represent error unions in IR?
- [ ] How to integrate with runtime?
- [ ] What's the minimum viable codegen?

---

**Status**: Ready to begin RED phase
**Next Action**: Create codegen test file and write first tests

