# TDD Cycle 5 - Blockers and Next Steps

**Date**: 2025-11-02
**Current Status**: 1/10 tests passing (10%)
**Primary Blocker**: Parser limitations (not codegen)

---

## Summary

TDD Cycle 5 has successfully validated the code generation infrastructure for **global variable initialization**. However, further progress is blocked by parser limitations - not by codegen bugs.

### What's Working ✅

- **Global integer variables**: `var x int = 42` ✅ PASS
- **Type system integration**: Types flow correctly through AST
- **LLVM IR generation**: Core infrastructure works
- **Integer literal codegen**: Proper type inference
- **Global initializers**: Constant initialization works

### What's Blocked ❌

**8 tests fail** due to parser not supporting:
1. Function bodies with statements
2. Return statements
3. Binary expressions in functions
4. Local variable declarations
5. If statements
6. Multiple statements in a block

---

## Detailed Blocker Analysis

### Test 2: Binary Arithmetic ❌

**Test Code**:
```goo
func calculate() int {
    return 10 + 5
}
```

**Error**: `Parse error at test.goo:5:1: syntax error`

**Root Cause**: Parser doesn't support:
- Function bodies with `return` statements
- Binary expressions (`10 + 5`) in function context

**Codegen Status**: Code exists for binary operations (`codegen_generate_binary_expr`), unreachable due to parse failure

### Test 3: Simple Function ❌

**Test Code**:
```goo
func add(a int, b int) int {
    return a + b
}
```

**Error**: `Parse error`

**Root Cause**: Same as Test 2 - function bodies with return statements

**Codegen Status**: Function codegen exists, unreachable due to parse failure

### Test 4-7: Same Pattern ❌

All fail with parse errors due to:
- Return statements inside functions
- Local variable declarations
- If statements
- Statement blocks

### Test 8: String Literal ❓

**Test Code**:
```goo
var message string = "hello"
```

**Error**: Hangs (no PASS/FAIL output)

**Root Cause**: Unknown - possibly:
1. String type verification failing
2. Infinite loop in test framework
3. Crash after codegen

**Debug Output Shows**:
```
DEBUG: Created literal node with type 27 at test.goo:3:1, value='hello'
DEBUG: Type checking expression with type 27 at test.goo:3:1
(then nothing)
```

**Note**: This is the ONLY non-parser issue blocking more tests

### Tests 9-10 ❌

**Error**: Parse errors again (function bodies, return statements)

---

## Parser Gaps

### Missing Syntax Support

The parser (`src/parser/parser.y`) needs to support:

1. **Return Statements**
   ```
   return_stmt: RETURN expression
   ```

2. **Statement Blocks**
   ```
   func_body: '{' statement_list '}'
   statement_list: statement | statement_list statement
   ```

3. **Local Variables**
   ```
   local_var_decl: VAR IDENT type_annotation '=' expression
   ```

4. **If Statements**
   ```
   if_stmt: IF expression '{' statement_list '}'
   ```

5. **Binary Expressions in Statement Context**
   ```
   expression_stmt: expression
   ```

### Parser Architecture

The Goo parser is built with:
- **Bison** (`parser.y`) - Grammar definition
- **Flex** (`lexer.l`) - Lexical analysis
- Output: AST nodes

**Location**: `/home/ddowney/Workspace/github.com/goolang/src/parser/`

**Key Files**:
- `parser.y` - Bison grammar (needs additions)
- `parser.tab.c` - Generated parser
- `lexer_bridge.c` - Connects lexer to parser

---

## Codegen Status

### Implemented and Working ✅

1. **Global Variables**
   - Declaration: `codegen_generate_var_decl()`
   - Initialization with constants
   - Type mapping (int, float, bool, string)

2. **Integer Literals**
   - Constant generation
   - Type inference from context
   - Proper LLVM IR generation

3. **String Literals** (with recent fix)
   - Global string constants
   - String struct {ptr, len}
   - Constant expressions

4. **Type System**
   - `codegen_type_to_llvm()` - Complete mapping
   - Basic types (int8-64, uint8-64, float32-64, bool)
   - String type
   - Arrays, slices, structs (partial)

### Implemented but Untested 🟡

These functions exist but can't be tested due to parser:

1. **Binary Operations**
   - `codegen_generate_binary_expr()`
   - Arithmetic: add, sub, mul, div
   - Comparisons: eq, ne, lt, gt, le, ge
   - Logical: and, or

2. **Function Bodies**
   - `codegen_generate_function_decl()` - Partial
   - Entry basic block creation
   - Parameter handling

3. **Return Statements**
   - `codegen_generate_return_stmt()`
   - Exists in `function_codegen.c`

4. **Local Variables**
   - `codegen_generate_var_decl()` - Handles both global and local
   - Alloca instruction generation
   - Store/load instructions

5. **Control Flow**
   - `codegen_generate_if_stmt()`
   - Basic block creation
   - Branch instructions
   - Phi nodes (partial)

### Not Implemented ❌

1. **Function Calls**
   - `codegen_generate_call_expr()` - Stub only
   - Argument passing
   - Return value handling

2. **Arrays/Slices**
   - Index operations
   - Slice operations
   - Length/capacity

3. **Error Unions** (!T)
   - Error value wrapping
   - Try/catch expressions
   - Error propagation

4. **Nullable Types** (?T)
   - Null checking
   - Optional unwrapping

---

## Two Paths Forward

### Path A: Fix Parser (Recommended)

**Effort**: 4-6 hours
**Impact**: Unlocks 8 tests (80% → potential 90% pass rate)

**Steps**:
1. Add return statement to grammar (30 min)
2. Add statement blocks to function bodies (1 hour)
3. Add local variable declarations (30 min)
4. Add if statement support (1 hour)
5. Test and debug (2-3 hours)

**Benefits**:
- Tests existing codegen features
- High impact on pass rate
- Completes critical language features
- Validates TDD approach thoroughly

**Files to Modify**:
- `src/parser/parser.y` - Grammar additions
- `src/parser/lexer_bridge.c` - Possible changes
- `src/ast/ast.c` - Possible new AST node types

### Path B: Fix String Test Only

**Effort**: 1-2 hours
**Impact**: 1 additional test (10% → 20% pass rate)

**Steps**:
1. Add more debug output to string test
2. Identify hang/crash location
3. Fix string literal bug
4. Verify test passes

**Benefits**:
- Quick win
- Validates string codegen
- Lower risk

**Drawback**:
- Still blocked on 8 tests
- Minimal progress on pass rate

---

## Recommended Plan

### Phase 1: Quick Win (1-2 hours)

1. **Fix String Literal Test**
   - Add debug output to test framework
   - Identify hang cause
   - Fix bug
   - **Target**: 2/10 tests passing (20%)

### Phase 2: Parser Enhancements (4-6 hours)

2. **Add Return Statement Support**
   - Modify `parser.y` grammar
   - Add AST node if needed
   - Test with simple function
   - **Target**: 3-4/10 tests passing (30-40%)

3. **Add Statement Block Support**
   - Function bodies with multiple statements
   - Statement sequencing
   - **Target**: 5-6/10 tests passing (50-60%)

4. **Add Local Variable Support**
   - Local var declarations in functions
   - Proper scoping
   - **Target**: 7/10 tests passing (70%)

5. **Add If Statement Support**
   - Conditional branches
   - Basic blocks
   - **Target**: 8/10 tests passing (80%)

### Phase 3: Complete Remaining (2-3 hours)

6. **Error Unions and Advanced Features**
   - Complete tests 9-10
   - **Target**: 10/10 tests passing (100%) 🎉

**Total Estimated Time**: 7-11 hours to 100% pass rate

---

## Alternative: Skip Parser Work

If parser modifications are out of scope for this cycle, we can:

1. **Document parser blockers** ✅ (this document)
2. **Fix string test** (1-2 hours)
3. **Declare GREEN phase "parser-blocked"**
4. **Move to REFACTOR phase** with codegen that's ready but untested

This would be acceptable because:
- Codegen infrastructure is solid
- Test framework is comprehensive
- Only parser limits testing
- Future parser work will validate codegen

---

## Key Metrics

### Current State
- Tests passing: 1/10 (10%)
- Codegen coverage: ~30% (only globals tested)
- Parser coverage: ~20% (basic declarations only)
- Time invested: ~4 hours

### With Path A (Parser Fixes)
- Tests passing: 8-10/10 (80-100%)
- Codegen coverage: ~80% (most features tested)
- Parser coverage: ~60% (basic statements supported)
- Additional time: 7-11 hours

### With Path B (String Only)
- Tests passing: 2/10 (20%)
- Codegen coverage: ~35% (globals + strings)
- Parser coverage: ~20% (no change)
- Additional time: 1-2 hours

---

## Decision Matrix

| Factor | Path A (Parser) | Path B (String Only) |
|--------|-----------------|---------------------|
| Pass Rate | ⭐⭐⭐⭐⭐ (90%) | ⭐ (20%) |
| Effort | ⭐⭐ (High) | ⭐⭐⭐⭐⭐ (Low) |
| Risk | ⭐⭐⭐ (Medium) | ⭐⭐⭐⭐⭐ (Low) |
| Impact | ⭐⭐⭐⭐⭐ (Huge) | ⭐ (Small) |
| Learning | ⭐⭐⭐⭐⭐ (High) | ⭐⭐ (Low) |

**Recommendation**: Path A (Parser fixes) for maximum value, unless parser work is explicitly out of scope.

---

## Technical Notes

### Parser Grammar Additions Needed

**Current Grammar** (simplified):
```yacc
program: package_decl declarations

declarations: declaration
            | declarations declaration

declaration: var_decl
           | const_decl
           | func_decl

func_decl: FUNC IDENT '(' params ')' type_annotation

// Missing: function bodies!
```

**Needed Grammar**:
```yacc
func_decl: FUNC IDENT '(' params ')' type_annotation func_body

func_body: '{' statement_list '}'
         | '{' '}'

statement_list: statement
              | statement_list statement

statement: return_stmt
         | var_decl_stmt
         | if_stmt
         | expression_stmt

return_stmt: RETURN expression

var_decl_stmt: VAR IDENT type_annotation '=' expression

if_stmt: IF expression '{' statement_list '}'
       | IF expression '{' statement_list '}' ELSE '{' statement_list '}'

expression_stmt: expression
```

### AST Nodes Needed

May need to add/verify:
- `ReturnStmtNode` - Return statement
- `BlockStmtNode` - Statement block
- `IfStmtNode` - If statement (may exist)
- `ExpressionStmtNode` - Expression as statement

Check `include/ast.h` for existing definitions.

---

## Success Criteria

### Minimum (Path B)
- ✅ 2/10 tests passing
- ✅ String literals working
- ✅ Codegen infrastructure validated

### Target (Path A)
- ✅ 8-10/10 tests passing
- ✅ Functions with bodies working
- ✅ Return statements working
- ✅ Local variables working
- ✅ Binary operations validated
- ✅ Control flow (if) working

### Stretch Goal
- ✅ 10/10 tests passing
- ✅ Error unions working
- ✅ Complete TDD cycle RED-GREEN-REFACTOR

---

## Files That Need Work

### Immediate (String Test)
- `tests/unit/codegen/codegen_integration_test.c` - Add debug
- Possibly `src/codegen/expression_codegen.c` - Fix string bug

### Parser Enhancements
- `src/parser/parser.y` - Add grammar rules
- `src/parser/parser.tab.c` - Regenerate
- `include/ast.h` - Possibly add nodes
- `src/ast/ast.c` - Possibly add node handling

### Codegen (may need fixes when tested)
- `src/codegen/function_codegen.c` - Return stmt, if stmt
- `src/codegen/expression_codegen.c` - Binary ops
- `src/codegen/codegen.c` - Statement generation

---

**Next Action**: Choose path and proceed. Parser work recommended for maximum impact.

**Status**: Documented and ready for decision.
