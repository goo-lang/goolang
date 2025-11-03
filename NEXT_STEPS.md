# Goo Compiler - Next Steps & Options

**Date:** 2025-11-03
**Current TDD Status:** 73% (22/30 tests passing)
**Session Status:** Parser limitations identified, excellent progress made

---

## 🎯 Current State Summary

### What's Working Great ✅

| Feature | Status | Tests | Notes |
|---------|--------|-------|-------|
| **Loops & Arrays** | ✅ 100% | 10/10 | Production ready! |
| **Multiple Returns** | ✅ 90% | 9/10 | Go-style error handling works |
| **Basic Codegen** | ✅ Working | - | Functions, expressions, variables |
| **Type Checking** | ✅ Working | - | Core types, expressions |
| **Parser (Core)** | ✅ Working | - | Basic Go syntax |

### What's Blocked ⚠️

| Feature | Status | Tests | Blocker |
|---------|--------|-------|---------|
| **Structs & Methods** | ⚠️ 30% | 3/10 | Composite literals parser conflict |
| **Named Returns** | ⚠️ Partial | 1/10 | Parser ambiguity (AST+codegen ready) |
| **Parser Basic Tests** | ⚠️ 41% | 5/12 | Old tests, semicolon issues |

### Parser Limitations Documented 📋

1. **Composite Literals** - `Person{}` creates 142 reduce/reduce conflicts
2. **Bare Identifiers in If** - Requires `if (ok) {}` not `if ok {}`
3. **Named Returns** - `(result int, ok bool)` ambiguous with `(int, bool)`

**Root Cause:** Context-sensitive parsing needs in LALR(1) grammar
**Solution:** Requires lexer feedback or GLR parser (major refactoring)

---

## 🚀 Recommended Next Steps (Prioritized)

### Option 1: Implement Switch Statements ⭐ RECOMMENDED
**Effort:** Medium (6-8 hours)
**Value:** High - Common control flow, no parser conflicts
**Complexity:** Straightforward

**What to implement:**
```go
switch x {
case 1:
    return "one"
case 2, 3:
    return "two or three"
default:
    return "other"
}
```

**Why this is good:**
- ✅ No parser ambiguity issues
- ✅ High-value Go feature
- ✅ Clear implementation path
- ✅ SWITCH token already exists in lexer
- ✅ Good learning for control flow codegen

**Tasks:**
1. Add switch_stmt grammar rule
2. Implement type checking for switch
3. Generate LLVM switch instruction or if-else chain
4. Create TDD test suite
5. Achieve 80%+ pass rate

---

### Option 2: Fix Parser Basic Tests
**Effort:** Low (2-3 hours)
**Value:** Medium - Increases test coverage
**Complexity:** Simple

**Current Status:** 41% (5/12 passing)

**Issues:**
- Tests missing semicolons: `x := 10` should be `x := 10;`
- Sequential statements failing (known parser issue)
- Error union syntax tests

**Tasks:**
1. Update tests to match current grammar
2. Add semicolons where required
3. Use parentheses workarounds for known issues
4. Document any remaining blockers

**Why this is good:**
- ✅ Quick wins
- ✅ Improves overall test coverage
- ✅ Validates parser changes

---

### Option 3: Implement Slices
**Effort:** Medium-High (8-10 hours)
**Value:** High - Essential Go feature
**Complexity:** Moderate

**What to implement:**
```go
s := []int{1, 2, 3}  // ❌ Blocked by composite literals
s := make([]int, 10)  // ✅ Can implement!
s[0] = 42
x := s[1:5]  // Slicing syntax
len(s)
cap(s)
```

**Why this is valuable:**
- ✅ Core Go data structure
- ✅ make() syntax avoids composite literal issue
- ✅ Builds on array implementation
- ✅ Enables more realistic programs

**Tasks:**
1. Add slice type to type system
2. Implement make() builtin for slices
3. Add slice indexing and slicing syntax
4. Generate LLVM code for slice operations
5. Implement len() and cap() for slices

---

### Option 4: Maps (Basic)
**Effort:** High (12-15 hours)
**Value:** High - Essential data structure
**Complexity:** High

**What to implement:**
```go
m := make(map[string]int)
m["key"] = 42
val := m["key"]
val, ok := m["key"]  // ✅ Multiple returns already work!
delete(m, "key")
len(m)
```

**Why this is valuable:**
- ✅ Essential Go feature
- ✅ Avoids composite literal syntax
- ✅ Multiple returns already implemented
- ✅ Real-world utility

**Challenges:**
- Runtime hash table implementation needed
- Memory management complexity
- Requires significant codegen work

---

### Option 5: Improve Error Handling
**Effort:** Medium (6-8 hours)
**Value:** Medium - Better compiler UX
**Complexity:** Moderate

**What to improve:**
- Better error messages with context
- Error recovery in parser
- Multiple error reporting
- Source location tracking

**Why this is valuable:**
- ✅ Improves developer experience
- ✅ Makes debugging easier
- ✅ Professional compiler quality

---

### Option 6: Defer Statements
**Effort:** Low-Medium (4-6 hours)
**Value:** Medium - Nice Go feature
**Complexity:** Moderate

**What to implement:**
```go
func example() {
    f := openFile()
    defer f.close()  // Runs at function exit
    // ... do work ...
}
```

**Why this is good:**
- ✅ DEFER token already exists
- ✅ Clear Go semantics
- ✅ Good for resource management
- ✅ Interesting codegen challenge

**Challenges:**
- Need to track defer stack
- Execute in reverse order
- Handle in all exit paths (return, panic)

---

## 📊 Decision Matrix

| Option | Effort | Value | No Parser Issues | Build on Existing |
|--------|--------|-------|------------------|-------------------|
| **Switch Statements** | 6-8h | ⭐⭐⭐ | ✅ | ✅ Control flow |
| **Fix Parser Tests** | 2-3h | ⭐⭐ | ✅ | ✅ Testing |
| **Slices** | 8-10h | ⭐⭐⭐ | ✅ | ✅ Arrays |
| **Maps** | 12-15h | ⭐⭐⭐ | ✅ | ❌ New territory |
| **Error Handling** | 6-8h | ⭐⭐ | ✅ | ✅ Compiler |
| **Defer** | 4-6h | ⭐⭐ | ✅ | ✅ Functions |

---

## 🎯 My Recommendation: Switch Statements

**Why:**
1. ✅ **No parser conflicts** - Avoids current blockers
2. ✅ **High value** - Commonly used Go feature
3. ✅ **Reasonable effort** - 6-8 hours
4. ✅ **Clear path** - Well-understood implementation
5. ✅ **Good learning** - Control flow codegen patterns
6. ✅ **SWITCH token exists** - Infrastructure ready

**Implementation Plan:**

### Phase 1: Parser (2 hours)
```yacc
switch_stmt:
    SWITCH expression LBRACE case_clause_list RBRACE
    | SWITCH LBRACE case_clause_list RBRACE  // no condition

case_clause_list:
    case_clause
    | case_clause_list case_clause

case_clause:
    CASE expression_list COLON statement_list
    | DEFAULT COLON statement_list
```

### Phase 2: AST & Type Checking (2 hours)
- Create SwitchStmtNode, CaseClauseNode
- Type check switch expression
- Ensure case expressions match switch type
- Handle fallthrough semantics

### Phase 3: Codegen (2-3 hours)
- Generate LLVM switch instruction
- Or generate if-else chain for complex cases
- Handle default case
- Implement break properly

### Phase 4: Tests (1-2 hours)
- Create TDD Cycle 10: Switch Statements
- Write 8-10 tests covering:
  - Basic switch with cases
  - Switch with multiple values per case
  - Switch without condition (like if-else)
  - Default case
  - Fallthrough
  - Break in switch
  - Nested switches

**Expected Outcome:** TDD Cycle 10 at 80%+ → Overall TDD at 80%+

---

## 📈 Long-term Parser Strategy

For composite literals and named returns:

### Option A: Lexer Feedback (Recommended)
- Mark type identifiers during lexing
- Requires symbol table integration
- **Effort:** 20-30 hours
- **Benefit:** Solves both issues permanently
- **When:** After 2-3 more features working

### Option B: GLR Parser
- Switch to GLR parser generator
- Handles ambiguity natively
- **Effort:** 15-25 hours
- **Benefit:** More flexible grammar
- **Risk:** Build system changes

### Option C: Restrict Syntax
- Require keyword for composite literals: `new Person{}`
- Use different syntax for named returns
- **Effort:** 5-10 hours
- **Benefit:** Quick fix
- **Cost:** Not Go-compatible

---

## 💡 Quick Wins Available

Before starting major work:

1. **Update parser basic tests** (2 hours) → 41% to 80%+
2. **Document all workarounds** (1 hour) → Better DX
3. **Commit TDD progress** (30 min) → Save state
4. **Clean up old TDD docs** (30 min) → Better organization

---

## 📝 Current Session Achievements

✅ TDD Cycle 8: 60% → 90%
✅ TDD Overall: → 73% (22/30)
✅ Discovered and documented parser limitations
✅ Implemented named returns foundation (AST + codegen)
✅ Created comprehensive documentation

**Files Created:**
- TDD_CYCLE_8_STATUS.md
- TDD_OVERALL_STATUS.md
- NAMED_RETURNS_ATTEMPT.md
- NEXT_STEPS.md (this file)

---

## 🎬 Ready to Start?

**Recommended:** Implement Switch Statements (TDD Cycle 10)

Would you like me to:
1. Start implementing switch statements?
2. Fix parser basic tests first (quick wins)?
3. Work on slices?
4. Something else from the options above?
