# Goo Language MVP - Critical Path Analysis

**Date**: 2025-11-02
**Goal**: Minimal Viable Product - Compile and run basic Go-compatible programs
**Status**: Planning

---

## MVP Definition

An MVP for Goo means:
- ✅ Compile basic Go programs with core features
- ✅ Generate working LLVM IR that executes correctly
- ✅ Support fundamental data structures and control flow
- ✅ Enable basic error handling patterns
- ✅ Allow code organization with methods and interfaces

---

## Critical Path Features (In Order)

### Priority 0: Foundation (COMPLETE ✅)
- ✅ Lexer and parser working
- ✅ Basic types (int, string, bool)
- ✅ Variables and assignment
- ✅ Functions and function calls
- ✅ Basic expressions (arithmetic, comparison, logical)
- ✅ If statements
- ✅ For loops, break, continue
- ✅ Arrays and array operations
- ✅ Built-in `len()` function

**Status**: 100% - TDD Cycle 6 complete

---

### Priority 1: Structs (IN PROGRESS 🟡)
**Why Critical**: Fundamental for any real data modeling

**Required Features**:
- ✅ Struct type declarations (parser done)
- 🔄 Struct type checking
- 🔄 Field access (read/write)
- 🔄 Struct literals
- 🔄 Struct codegen
- ⏸️ Nested structs
- ⏸️ Struct assignment/copying

**Current State**: Parser works (30% done)
**Estimated Time**: 12-15 hours
**Blocking**: Methods, interfaces

**Next Actions**:
1. Implement struct type registration
2. Implement field access type checking
3. Add struct literal syntax
4. Generate LLVM struct types and GEP instructions

---

### Priority 2: Multiple Return Values (NOT STARTED ❌)
**Why Critical**: Essential for Go error handling pattern

**Required Features**:
- ⏸️ Function signatures with multiple returns: `func f() (int, error)`
- ⏸️ Multiple assignment: `a, b := f()`
- ⏸️ Tuple destructuring
- ⏸️ Underscore for unused returns: `_, err := f()`
- ⏸️ Type checking for tuple types
- ⏸️ LLVM struct return values

**Estimated Time**: 8-10 hours
**Blocking**: Idiomatic error handling

**Dependencies**: None (can start after structs)

---

### Priority 3: Methods (NOT STARTED ❌)
**Why Critical**: Required for organizing code and interfaces

**Required Features**:
- ⏸️ Method declarations: `func (t Type) Method()`
- ⏸️ Value receivers vs pointer receivers
- ⏸️ Method calls: `obj.Method()`
- ⏸️ Method name mangling/symbol resolution
- ⏸️ Implicit receiver parameter

**Estimated Time**: 6-8 hours
**Blocking**: Interfaces, OOP patterns

**Dependencies**: Structs (Priority 1)

---

### Priority 4: Switch Statements (NOT STARTED ❌)
**Why Critical**: Common control flow, cleaner than if/else chains

**Required Features**:
- ⏸️ Expression switch: `switch x { case 1: ... }`
- ⏸️ Fallthrough behavior
- ⏸️ Multiple case values: `case 1, 2, 3:`
- ⏸️ Default case
- ⏸️ Type switch (lower priority): `switch v := x.(type)`

**Estimated Time**: 6-8 hours
**Blocking**: Nothing critical

**Dependencies**: None

---

### Priority 5: Range Loops (NOT STARTED ❌)
**Why Critical**: Idiomatic Go iteration pattern

**Required Features**:
- ⏸️ Range over arrays: `for i, v := range arr`
- ⏸️ Range over slices
- ⏸️ Range with single variable (index only)
- ⏸️ Range with underscore: `for _, v := range arr`
- ⏸️ Range over strings (UTF-8 runes)

**Estimated Time**: 6-8 hours
**Blocking**: Nothing critical, but very common

**Dependencies**: Arrays (done), slices (partially done)

---

### Priority 6: Slices (PARTIAL - 30% ✅)
**Why Critical**: Most common collection type in Go

**Required Features**:
- ✅ Slice type declarations
- ⏸️ Slice literals: `[]int{1, 2, 3}`
- ⏸️ Slicing syntax: `arr[1:3]`, `arr[:]`
- ⏸️ `append()` built-in
- ⏸️ `copy()` built-in
- ⏸️ `cap()` built-in
- ⏸️ Slice runtime (length, capacity, data pointer)

**Estimated Time**: 10-12 hours
**Blocking**: Range loops need slices

**Dependencies**: Arrays (done)

---

### Priority 7: Interfaces (NOT STARTED ❌)
**Why Critical**: Essential for polymorphism and abstraction

**Required Features**:
- ⏸️ Interface declarations
- ⏸️ Interface method signatures
- ⏸️ Implicit interface implementation
- ⏸️ Interface values (type + data pointer)
- ⏸️ Dynamic dispatch
- ⏸️ Type assertions: `x.(Type)`

**Estimated Time**: 12-15 hours
**Blocking**: Polymorphism, standard library patterns

**Dependencies**: Methods (Priority 3)

---

### Priority 8: String Operations (PARTIAL - 20% ✅)
**Why Critical**: Essential for any I/O or text processing

**Required Features**:
- ✅ String type exists
- ⏸️ String literals with escapes: `\n`, `\t`, `\"`
- ⏸️ String concatenation: `+`
- ⏸️ String indexing: `s[i]`
- ⏸️ String slicing: `s[1:3]`
- ⏸️ String comparison
- ⏸️ String runtime (length, data pointer)

**Estimated Time**: 6-8 hours
**Blocking**: Basic I/O and formatting

**Dependencies**: Slices (for string slice representation)

---

### Priority 9: Pointers (PARTIAL - 20% ✅)
**Why Critical**: Required for efficient struct passing and mutation

**Required Features**:
- ✅ Pointer types in grammar
- ⏸️ Address-of operator: `&x`
- ⏸️ Dereference operator: `*ptr`
- ⏸️ Nil pointers
- ⏸️ Pointer comparison
- ⏸️ Pointer arithmetic (intentionally limited)

**Estimated Time**: 4-6 hours
**Blocking**: Efficient struct operations

**Dependencies**: None

---

### Priority 10: Maps (PARTIAL - 20% ✅)
**Why Critical**: Common data structure, but lower priority

**Required Features**:
- ✅ Map type declarations
- ⏸️ Map literals: `map[string]int{"a": 1}`
- ⏸️ Map access: `m[key]`
- ⏸️ Map assignment: `m[key] = value`
- ⏸️ Map deletion: `delete(m, key)`
- ⏸️ Map existence check: `v, ok := m[key]`
- ⏸️ Map runtime (hash table)

**Estimated Time**: 10-12 hours
**Blocking**: Nothing critical for MVP

**Dependencies**: Multiple return values (for existence check)

---

## Time Estimates Summary

| Priority | Feature | Time | Current % | Status |
|----------|---------|------|-----------|--------|
| 0 | Foundation | - | 100% | ✅ COMPLETE |
| 1 | **Structs** | 12-15h | 30% | 🟡 IN PROGRESS |
| 2 | **Multiple Returns** | 8-10h | 0% | ❌ NOT STARTED |
| 3 | **Methods** | 6-8h | 0% | ❌ NOT STARTED |
| 4 | **Switch** | 6-8h | 0% | ❌ NOT STARTED |
| 5 | **Range** | 6-8h | 0% | ❌ NOT STARTED |
| 6 | **Slices** | 10-12h | 30% | ❌ NOT STARTED |
| 7 | **Interfaces** | 12-15h | 0% | ❌ NOT STARTED |
| 8 | **Strings** | 6-8h | 20% | ❌ NOT STARTED |
| 9 | **Pointers** | 4-6h | 20% | ❌ NOT STARTED |
| 10 | **Maps** | 10-12h | 20% | ❌ NOT STARTED |

**Total Remaining**: ~80-104 hours (~10-13 full work days)

---

## Critical Path Dependencies

```
Foundation (DONE)
    ↓
1. Structs (IN PROGRESS)
    ↓
    ├─→ 3. Methods ─→ 7. Interfaces
    ├─→ 9. Pointers
    └─→ 6. Slices ─→ 5. Range ─→ 8. Strings

2. Multiple Returns (parallel to structs)

4. Switch (independent)

10. Maps (lowest priority)
```

---

## Recommended Implementation Order

### Phase 1: Core Data Structures (3-4 weeks)
1. ✅ **Complete Structs** (12-15h) - TDD Cycle 7
2. **Multiple Return Values** (8-10h) - TDD Cycle 8
3. **Methods** (6-8h) - TDD Cycle 9
4. **Pointers** (4-6h) - TDD Cycle 10

**Milestone**: Can define data structures with methods

### Phase 2: Control Flow & Collections (2-3 weeks)
5. **Switch Statements** (6-8h) - TDD Cycle 11
6. **Slices (Complete)** (10-12h) - TDD Cycle 12
7. **Range Loops** (6-8h) - TDD Cycle 13
8. **String Operations** (6-8h) - TDD Cycle 14

**Milestone**: Can iterate and manipulate collections

### Phase 3: Polymorphism (2 weeks)
9. **Interfaces** (12-15h) - TDD Cycle 15

**Milestone**: Can write polymorphic code

### Phase 4: Polish (1 week)
10. **Maps (Complete)** (10-12h) - TDD Cycle 16

**Milestone**: Full MVP feature set

---

## MVP Success Criteria

A working MVP should be able to compile and run this program:

```goo
package main

type Person struct {
    name string;
    age int;
}

func (p Person) greet() string {
    return "Hello, my name is " + p.name;
}

func findPerson(people []Person, name string) (Person, bool) {
    for _, p := range people {
        if p.name == name {
            return p, true;
        }
    }
    return Person{}, false;
}

func main() {
    people := []Person{
        Person{name: "Alice", age: 30},
        Person{name: "Bob", age: 25},
    };

    person, found := findPerson(people, "Alice");
    if found {
        println(person.greet());
    } else {
        println("Person not found");
    }
}
```

---

## Current Focus: Complete Structs (Priority 1)

**Next Immediate Steps**:
1. Implement struct type system (4-5 hours)
2. Implement struct codegen (5-6 hours)
3. Add struct literal syntax (2-3 hours)

**Target**: Get TDD Cycle 7 to 80%+ pass rate (8/10 tests)

---

**Total MVP Estimate**: 80-104 hours (~2-3 months at 10 hours/week)
**Current Progress**: ~15% complete
**Next Milestone**: Structs complete (30% → 50% overall progress)
