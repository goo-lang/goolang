# Named Return Parameters - Implementation Attempt

## Goal
Implement Go-style named return parameters to complete TDD Cycle 8 to 100%.

Example:
```go
func divide(a int, b int) (result int, ok bool) {
    if b == 0 {
        result = 0;
        ok = false;
        return result, ok;
    }
    result = a / b;
    ok = true;
    return result, ok;
}
```

## What Was Implemented

### ✅ AST Updates (Completed)
- Added `named_returns` field to `FuncDeclNode` in `include/ast.h`
- Updated `ast_func_decl_new()` to initialize the new field

### ✅ Codegen Updates (Completed)
- Added code in `src/codegen/function_codegen.c` to create local variables for named returns
- Named returns are automatically declared at function entry
- Can be assigned and used like regular local variables

**Code location:** Lines 254-283 in `function_codegen.c`

### ❌ Parser Updates (BLOCKED)
**Problem:** Severe parser ambiguity causing 142 reduce/reduce conflicts

#### The Ambiguity

The parser cannot distinguish between:
1. **Anonymous returns:** `(int, bool)`
2. **Named returns:** `(result int, ok bool)`

Both patterns start with `LPAREN identifier` and the parser must choose:
- Reduce `identifier` as a type name → anonymous returns
- Shift to wait for second `identifier` → named returns

#### Grammar Conflict Details

```yacc
opt_func_result:
    | LPAREN type_list RPAREN          // (int, bool)
    | LPAREN func_params RPAREN        // (result int, ok bool)  ← CONFLICT!

type_list:
    type COMMA type                     // type → identifier

func_params:
    identifier type COMMA identifier type   // Overlaps with type_list
```

**Result:** 142 reduce/reduce conflicts, bison refuses to generate parser

#### Attempts Made

1. **Reuse func_params rule** - Created massive conflicts
2. **Add %expect directives** - %expect-rr only works for GLR parsers
3. **Lookahead disambiguation** - LALR(1) parser can't look far enough ahead

## Why It's Hard

This is a **context-sensitive parsing** problem in a **context-free grammar**:

- Need to know if `result` is a variable name or type name
- Requires distinguishing `(result int)` from `(int)`
- LR parsers decide based on local context only

## Possible Solutions (Ordered by Difficulty)

### Option 1: Accept Conflicts & Test (Quick)
- Use `%expect 150` to suppress errors
- Test if default parser behavior works
- **Risk:** May parse incorrectly in edge cases

### Option 2: GLR Parser (Medium)
- Switch to GLR parser generator
- Handles ambiguity by exploring multiple parses
- **Issue:** Requires changing build system

### Option 3: Lexer Feedback (Complex)
- Mark identifiers as TYPE vs VARIABLE during lexing
- Requires symbol table integration
- **Complexity:** Major refactoring, Go compiler does this

### Option 4: Semantic Analysis (Complex)
- Parse ambiguously, resolve during type checking
- Requires AST rewriting capability
- **Complexity:** High, affects many subsystems

### Option 5: Restrict Syntax (Simple)
- Require keyword: `returns(result int, ok bool)`
- No ambiguity with clear marker
- **Issue:** Not Go-compatible

## Recommended Path Forward

**Short-term:**
Document named returns as unsupported, keep TDD Cycle 8 at 90%

**Medium-term:**
Try Option 1 (accept conflicts) with extensive testing to see if it works in practice

**Long-term:**
Implement Option 3 (lexer feedback) as part of larger parser improvement effort

## Current Status

- **TDD Cycle 8:** 90% (9/10 tests)
- **Blocking test:** `test_named_return_parameters`
- **AST:** Ready for named returns ✅
- **Codegen:** Ready for named returns ✅
- **Parser:** Blocked by ambiguity ❌

## Files Modified (Kept)

- `include/ast.h` - Added `named_returns` field
- `src/ast/ast.c` - Initialize `named_returns` to NULL
- `src/codegen/function_codegen.c` - Generate named return variables

## Files Reverted

- `src/parser/parser.y` - Parser changes reverted due to conflicts

## Testing

Cannot test until parser issue is resolved.

## Conclusion

Named returns are 2/3 implemented (AST + codegen done, parser blocked). The parser ambiguity is a fundamental limitation of LALR parsing for this Go feature. Achieving 100% on TDD Cycle 8 requires either:
1. Accepting parser conflicts and testing behavior
2. Switching to GLR parser
3. Implementing lexer feedback system

Given current progress (90%, 22/30 overall TDD), recommend continuing with other features and revisiting named returns as part of larger parser improvements.
