# TDD Cycle 7 - Structs and Methods

**Date**: 2025-11-02
**Focus**: High-Priority Core Features
**Status**: 🔴 RED PHASE - Planning
**Target**: Implement structs, methods, and basic interfaces

---

## Executive Summary

TDD Cycle 7 focuses on implementing **structs and methods**, which are fundamental to Go and required for most production code. This cycle will make Goo capable of expressing real-world data structures and object-oriented patterns.

### Goals

1. ✅ **Struct Type Declarations** - Define custom data types
2. ✅ **Struct Field Access** - Read/write struct fields
3. ✅ **Struct Literals** - Initialize structs with values
4. ✅ **Methods** - Attach functions to types
5. ⚠️ **Embedded Structs** - Struct composition (stretch goal)

---

## High-Priority Features Roadmap

From the analysis, these are the high-priority features:

| Priority | Feature | Cycle | Rationale |
|----------|---------|-------|-----------|
| 1 | **Structs** | Cycle 7 | Foundation for all data modeling |
| 2 | **Methods** | Cycle 7 | Required for idiomatic Go code |
| 3 | **Interfaces** | Cycle 8 | Essential for polymorphism |
| 4 | **Switch statements** | Cycle 9 | Common control flow |
| 5 | **Range loops** | Cycle 9 | Idiomatic iteration |
| 6 | **Multiple return values** | Cycle 8 | Go convention for errors |

**This Cycle Focus**: Structs + Methods (Priority 1 & 2)

---

## Test Plan - Structs

### Test 1: Basic Struct Declaration
```goo
package main

type Person struct {
    name string;
    age int;
}

func test() Person {
    var p Person;
    return p;
}
```
**Expected**: Compile successfully, return zero-initialized struct

### Test 2: Struct Field Assignment
```goo
package main

type Person struct {
    name string;
    age int;
}

func test() int {
    var p Person;
    p.age = 25;
    return p.age;
}
```
**Expected**: Return 25

### Test 3: Struct Literal - Zero Values
```goo
package main

type Person struct {
    name string;
    age int;
}

func test() Person {
    p := Person{};
    return p;
}
```
**Expected**: Return struct with zero values

### Test 4: Struct Literal - With Values
```goo
package main

type Person struct {
    name string;
    age int;
}

func test() int {
    p := Person{name: "Alice", age: 30};
    return p.age;
}
```
**Expected**: Return 30

### Test 5: Struct as Function Parameter
```goo
package main

type Person struct {
    name string;
    age int;
}

func get_age(p Person) int {
    return p.age;
}

func test() int {
    p := Person{age: 25};
    return get_age(p);
}
```
**Expected**: Return 25

### Test 6: Struct as Function Return Value
```goo
package main

type Person struct {
    name string;
    age int;
}

func create_person(age int) Person {
    return Person{age: age};
}

func test() int {
    p := create_person(40);
    return p.age;
}
```
**Expected**: Return 40

### Test 7: Nested Struct Access
```goo
package main

type Address struct {
    city string;
    zip int;
}

type Person struct {
    name string;
    address Address;
}

func test() int {
    var p Person;
    p.address.zip = 12345;
    return p.address.zip;
}
```
**Expected**: Return 12345

### Test 8: Multiple Struct Types
```goo
package main

type Person struct {
    age int;
}

type Company struct {
    size int;
}

func test() int {
    p := Person{age: 25};
    c := Company{size: 100};
    return p.age + c.size;
}
```
**Expected**: Return 125

---

## Test Plan - Methods

### Test 9: Basic Method Declaration
```goo
package main

type Counter struct {
    value int;
}

func (c Counter) get_value() int {
    return c.value;
}

func test() int {
    c := Counter{value: 42};
    return c.get_value();
}
```
**Expected**: Return 42

### Test 10: Pointer Receiver Method
```goo
package main

type Counter struct {
    value int;
}

func (c *Counter) increment() {
    c.value = c.value + 1;
}

func test() int {
    c := Counter{value: 10};
    c.increment();
    return c.value;
}
```
**Expected**: Return 11

---

## Implementation Phases

### Phase 1: Parser Support (Grammar)

**Files to Modify**:
- `src/parser/parser.y` - Add struct grammar rules

**Grammar Rules Needed**:
```yacc
type_decl:
    TYPE IDENTIFIER struct_type
    ;

struct_type:
    STRUCT LBRACE field_decl_list RBRACE
    ;

field_decl_list:
    field_decl
    | field_decl_list field_decl
    ;

field_decl:
    IDENTIFIER type SEMICOLON
    ;

struct_literal:
    type_name LBRACE field_value_list RBRACE
    | type_name LBRACE RBRACE
    ;

field_value_list:
    field_value
    | field_value_list COMMA field_value
    ;

field_value:
    IDENTIFIER COLON expression
    ;

method_decl:
    FUNC LPAREN receiver RPAREN IDENTIFIER func_signature block
    ;

receiver:
    IDENTIFIER type
    | IDENTIFIER STAR type
    ;
```

### Phase 2: AST Nodes

**Files to Modify**:
- `include/ast.h` - Add struct/method AST node types
- `src/ast/ast.c` - Implement constructors/destructors

**AST Nodes Needed**:
```c
// Struct type declaration
typedef struct StructTypeNode {
    ASTNode base;
    char** field_names;
    Type** field_types;
    size_t field_count;
} StructTypeNode;

// Struct literal
typedef struct StructLiteralNode {
    ASTNode base;
    Type* struct_type;
    char** field_names;
    ASTNode** field_values;
    size_t field_count;
} StructLiteralNode;

// Method declaration
typedef struct MethodDeclNode {
    ASTNode base;
    char* receiver_name;
    Type* receiver_type;
    int is_pointer_receiver;
    char* method_name;
    FunctionDeclNode* func_decl;
} MethodDeclNode;

// Field access (selector expression)
typedef struct SelectorNode {
    ASTNode base;
    ASTNode* object;
    char* field_name;
} SelectorNode;
```

### Phase 3: Type System

**Files to Modify**:
- `include/types.h` - Add struct type support
- `src/types/types.c` - Implement struct type creation
- `src/types/type_checker.c` - Type check struct operations
- `src/types/expression_checker.c` - Type check field access

**Type System Changes**:
```c
// TYPE_STRUCT already exists in TypeKind enum
// Need to populate the struct data properly

typedef struct Type {
    TypeKind kind;
    // ...
    union {
        // ...
        struct {
            char** field_names;
            Type** field_types;
            size_t field_count;
            char* struct_name;
        } struct_type;
        // ...
    } data;
} Type;

// Type checking functions needed:
Type* type_check_struct_decl(TypeChecker* checker, ASTNode* decl);
Type* type_check_struct_literal(TypeChecker* checker, ASTNode* literal);
Type* type_check_selector_expr(TypeChecker* checker, ASTNode* selector);
Type* type_check_method_decl(TypeChecker* checker, ASTNode* method);
```

### Phase 4: Code Generation

**Files to Modify**:
- `src/codegen/type_mapping.c` - Map struct types to LLVM
- `src/codegen/expression_codegen.c` - Generate field access code
- `src/codegen/codegen.c` - Generate struct literals

**LLVM Type Mapping**:
```c
LLVMTypeRef codegen_struct_to_llvm(CodeGenerator* codegen, Type* struct_type) {
    // Create LLVM struct type
    LLVMTypeRef* field_types = malloc(sizeof(LLVMTypeRef) * field_count);
    for (size_t i = 0; i < field_count; i++) {
        field_types[i] = codegen_type_to_llvm(codegen, struct_type->data.struct_type.field_types[i]);
    }
    return LLVMStructTypeInContext(codegen->context, field_types, field_count, 0);
}
```

**Field Access Codegen**:
```c
ValueInfo* codegen_generate_selector_expr(CodeGenerator* codegen, TypeChecker* checker, ASTNode* expr) {
    SelectorNode* selector = (SelectorNode*)expr;

    // Generate object
    ValueInfo* object = codegen_generate_expression(codegen, checker, selector->object);

    // Find field index
    int field_index = find_field_index(object->goo_type, selector->field_name);

    // Generate GEP to field
    LLVMValueRef indices[2] = {
        LLVMConstInt(LLVMInt32Type(), 0, 0),
        LLVMConstInt(LLVMInt32Type(), field_index, 0)
    };
    LLVMValueRef field_ptr = LLVMBuildGEP2(
        codegen->builder,
        codegen_type_to_llvm(codegen, object->goo_type),
        object->llvm_value,
        indices, 2, "field_ptr"
    );

    // Load field value
    LLVMValueRef field_value = LLVMBuildLoad2(
        codegen->builder,
        codegen_type_to_llvm(codegen, field_type),
        field_ptr, "field_value"
    );

    return value_info_new(field_ptr, field_value, field_type);
}
```

---

## Success Criteria

### Minimum Success (50% Pass Rate - 5/10 tests)
- ✅ Struct type declarations parse correctly
- ✅ Struct field access works (read/write)
- ✅ Zero-value struct literals work
- ✅ Structs as function parameters work
- ✅ Structs as return values work

### Target Success (80% Pass Rate - 8/10 tests)
- ✅ All minimum criteria
- ✅ Struct literals with field values work
- ✅ Nested struct access works
- ✅ Multiple struct types work

### Excellent Success (100% Pass Rate - 10/10 tests)
- ✅ All target criteria
- ✅ Basic methods work (value receivers)
- ✅ Pointer receiver methods work

---

## Risk Assessment

### High Risk
1. **LLVM struct layout** - Must match Go ABI for interop
2. **Method dispatch** - Name mangling and symbol resolution
3. **Parser conflicts** - Struct literal vs block ambiguity

### Medium Risk
1. **Field initialization order** - Must match Go semantics
2. **Nested struct codegen** - Recursive GEP instructions
3. **Method receivers** - Implicit parameter handling

### Low Risk
1. **Basic struct operations** - Well-understood in LLVM
2. **Type checking** - Clear semantics from Go spec
3. **Zero initialization** - Already working for arrays

---

## Estimated Time

| Phase | Estimated Time | Complexity |
|-------|----------------|------------|
| **Planning & Tests** | 2 hours | Low |
| **Parser Grammar** | 3-4 hours | Medium |
| **AST Nodes** | 2-3 hours | Low |
| **Type System** | 4-5 hours | Medium-High |
| **Code Generation** | 5-6 hours | High |
| **Testing & Debug** | 3-4 hours | Medium |
| **Documentation** | 2 hours | Low |

**Total Estimated**: 21-26 hours

**Target**: Complete in 3-4 development sessions

---

## Dependencies

### Completed (Cycle 6)
- ✅ Basic types (int, string)
- ✅ Functions
- ✅ Variables
- ✅ Assignment operators
- ✅ Return statements

### Required for This Cycle
- ✅ Type declarations framework
- ✅ Expression evaluation
- ✅ LLVM type mapping
- ✅ Memory allocation (alloca)

### Will Enable (Future Cycles)
- 🔜 Interfaces (Cycle 8)
- 🔜 Type assertions
- 🔜 Polymorphism
- 🔜 More idiomatic Go patterns

---

## References

- [Go Language Specification - Struct Types](https://go.dev/ref/spec#Struct_types)
- [Go Language Specification - Method Declarations](https://go.dev/ref/spec#Method_declarations)
- [LLVM Documentation - Struct Types](https://llvm.org/docs/LangRef.html#structure-type)
- TDD Cycle 6 Complete - Lessons learned on initialization

---

**Status**: 🔴 RED PHASE - Ready to write tests
**Next Step**: Create test file `tests/unit/codegen/structs_methods_test.c`
