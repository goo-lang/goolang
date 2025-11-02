# TDD Cycle 6 - RED Phase

**Date**: 2025-11-02
**Focus**: Advanced Control Flow and Data Structures
**Status**: 🔴 RED PHASE - Creating Tests

---

## Cycle Goals

Building on Cycle 5's success with basic code generation, Cycle 6 targets:

1. **Loop Constructs**
   - For loops (C-style and range-based)
   - Loop control (break, continue)

2. **Array and Slice Operations**
   - Array declarations
   - Array indexing
   - Slice creation and operations
   - Bounds checking

3. **Advanced Features**
   - Multiple return values (if time permits)
   - Struct basics (if time permits)

---

## Test Strategy

### Phase 1: For Loops (Priority 1)
- Test 1: Basic C-style for loop with counter
- Test 2: For-range over array
- Test 3: For loop with break
- Test 4: For loop with continue

### Phase 2: Arrays (Priority 2)
- Test 5: Array declaration and initialization
- Test 6: Array element access and assignment
- Test 7: Array length

### Phase 3: Slices (Priority 3)
- Test 8: Slice creation from array
- Test 9: Slice operations (append, copy)
- Test 10: Bounds checking validation

---

## Expected Failures

Based on code inspection:
- For loops: Codegen likely exists but may have bugs
- Arrays: Basic support exists, may need fixes
- Slices: Complex runtime support, may be incomplete
- Break/continue: May not be fully implemented

---

## Success Criteria

**Minimum**: 5/10 tests passing (50%)
- Basic for loop
- Array declaration
- Array access
- One advanced feature

**Target**: 8/10 tests passing (80%)
**Stretch**: 10/10 tests passing (100%)

---

## Next Steps

1. Create test file with 10 integration tests
2. Run tests and document all failures
3. Analyze failure patterns
4. Prioritize fixes by impact
5. Move to GREEN phase
