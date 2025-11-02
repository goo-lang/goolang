# TDD Cycle 8 - Session 1: Multiple Return Values

**Date**: 2025-11-03
**Status**: 🔴 Red Phase - Tests Created, Implementation In Progress
**Pass Rate**: 0/10 (0%)

---

## Session Goals

Implement Go-style multiple return values to enable idiomatic error handling patterns like:
```goo
func divide(a int, b int) (int, bool) {
    if b == 0 {
        return 0, false;
    }
    return a / b, true;
}

result, ok := divide(10, 2);
```

---

## Test Suite Created ✅

**File**: `tests/unit/codegen/multiple_returns_test.c`
**Tests**: 10 comprehensive tests covering:

1. ✗ Multiple return declaration - `func f() (int, bool)`
2. ✗ Multiple assignment - `a, b := get_values()`
3. ✗ Error handling pattern - `result, ok := divide(10, 2)`
4. ✗ Underscore for unused returns - `_, b := get_values()`
5. ✗ Different types - `(int, bool, int)`
6. ✗ Named return parameters - `func f() (result int, ok bool)`
7. ✗ Multiple return paths - Different branches returning tuples
8. ✗ Passing multiple returns - Using tuple values as arguments
9. ✗ Simple two value return - Basic swap function
10. ✗ Multiple returns in expressions - Arithmetic with tuple values

**Current Result**: All 10 tests failing with parse errors ✅ (Expected for Red phase)

---

## Implementation Progress

### ✅ Completed

1. **AST Enum Addition**
   - Added `AST_TUPLE_TYPE` to enum in `include/ast.h:66`

2. **Tuple Type Node Struct**
   - Added `TupleTypeNode` struct in `include/ast.h:471-476`
   ```c
   typedef struct {
       ASTNode base;
       struct ASTNode** element_types;
       size_t element_count;
   } TupleTypeNode;
   ```

3. **Constructor Declaration**
   - Added `ast_tuple_type_new()` declaration in `include/ast.h:952`

4. **Makefile Target**
   - Added `test-multiple-returns` target in `Makefile:582-603`

### 🔄 In Progress

5. **AST Constructor Implementation**
   - Need to implement `ast_tuple_type_new()` in AST source file

6. **Parser Grammar**
   - Need to modify `opt_func_result` rule to support:
     - `LPAREN type_list RPAREN` for multiple returns
     - Comma-separated type lists

### ⏸️ Pending

7. **Type System**
   - Add `TYPE_TUPLE` to type checker
   - Implement tuple type checking
   - Handle multiple assignment type checking

8. **Code Generation**
   - Generate LLVM struct for multiple return values
   - Extract tuple elements
   - Support underscore for ignored values

9. **Parser - Multiple Assignment**
   - Add syntax for `a, b := f()`
   - Handle identifier lists on left side

---

## Technical Design

### Return Value Strategy

Multiple returns will be implemented as LLVM struct returns:
```c
// Goo: func f() (int, bool)
// LLVM: { i32, i1 } @f()

// Goo: a, b := f()
// LLVM:
//   %ret = call { i32, i1 } @f()
//   %a = extractvalue { i32, i1 } %ret, 0
//   %b = extractvalue { i32, i1 } %ret, 1
```

### Parser Changes Needed

```yacc
opt_func_result:
    /* empty */ { $$ = NULL; }
    | type { $$ = $1; }
    | LPAREN type RPAREN { $$ = $2; }
    | LPAREN type_list RPAREN {
        // Create tuple type node
        $$ = ast_tuple_type_new(...);
    }
    ;

type_list:
    type { ... }
    | type_list COMMA type { ... }
    ;
```

---

## Next Steps (Priority Order)

1. ✅ Implement `ast_tuple_type_new()` constructor
2. ✅ Add parser grammar for `type_list` and tuple returns
3. ✅ Add `TYPE_TUPLE` to type system
4. ✅ Implement basic tuple type checking
5. ✅ Add codegen for returning tuples (structs)
6. ✅ Add codegen for receiving tuples (extract values)
7. ✅ Add multiple assignment syntax `a, b := f()`
8. ✅ Support underscore `_` for ignored returns
9. ⏸️ Named return parameters (lower priority)
10. ⏸️ Advanced tuple features (lower priority)

---

## Time Estimates

- **Parser + AST**: 2-3 hours
- **Type checking**: 2-3 hours
- **Code generation**: 3-4 hours
- **Multiple assignment**: 1-2 hours
- **Testing & debugging**: 1-2 hours

**Total Estimated**: 9-14 hours
**Target for 80%**: Get 8/10 tests passing

---

## Files Modified So Far

1. `include/ast.h` - Added AST_TUPLE_TYPE enum and TupleTypeNode struct
2. `Makefile` - Added test-multiple-returns target
3. `tests/unit/codegen/multiple_returns_test.c` - Created (447 lines)

---

## Session 1 Summary

**Status**: 🔴 Red Phase Complete
**Time Spent**: ~1 hour
**Achievement**: Test suite created, AST foundation laid
**Next Session**: Implement parser grammar and complete AST constructor

---

**Note**: This is a complex feature requiring changes across parser, type checker, and codegen. We're taking a systematic TDD approach, starting with failing tests and incrementally adding functionality.
