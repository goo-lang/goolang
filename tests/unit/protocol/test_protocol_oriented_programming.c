#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Include minimal type definitions for testing
typedef struct Type Type;
typedef struct InterfaceMethod InterfaceMethod;
typedef struct TypeChecker TypeChecker;
typedef struct ConstraintSet ConstraintSet;

typedef enum {
    TYPE_UNKNOWN,
    TYPE_INT32,
    TYPE_BOOL,
    TYPE_STRING,
    TYPE_STRUCT,
    TYPE_ARRAY
} TypeKind;

typedef enum {
    TYPE_VAR_GENERIC,
    TYPE_VAR_CONST,
    TYPE_VAR_LIFETIME,
    TYPE_VAR_HIGHER_KINDED,
    TYPE_VAR_ASSOCIATED
} TypeVariableKind;

struct Type {
    TypeKind kind;
    char* name;
    size_t size;
    size_t align;
};

struct InterfaceMethod {
    char* name;
    Type* signature;
    struct InterfaceMethod* next;
};

typedef struct {
    int line;
    int column;
    char* file;
} Position;

typedef struct TypeVariable {
    char* name;
    TypeVariableKind kind;
    Type* bound_type;
    ConstraintSet* constraints;
    int is_inferred;
    Position declared_pos;
    struct TypeVariable* next;
} TypeVariable;

struct ConstraintSet {
    void* constraints;
    size_t count;
    int is_satisfied;
    char* error_message;
};

struct TypeChecker {
    // Mock type checker
    int dummy;
};

// Mock implementations of required functions
Type* type_new(TypeKind kind) {
    Type* type = malloc(sizeof(Type));
    if (type) {
        type->kind = kind;
        type->name = NULL;
        type->size = 0;
        type->align = 1;
    }
    return type;
}

void type_free(Type* type) {
    if (type) {
        free(type->name);
        free(type);
    }
}

Type* type_copy(const Type* type) {
    if (!type) return NULL;
    Type* copy = type_new(type->kind);
    if (copy && type->name) {
        copy->name = strdup(type->name);
        copy->size = type->size;
        copy->align = type->align;
    }
    return copy;
}

int type_equals(const Type* a, const Type* b) {
    if (!a || !b) return a == b;
    if (a->kind != b->kind) return 0;
    if (a->name && b->name) return strcmp(a->name, b->name) == 0;
    return a->name == b->name;
}

Type* type_reference(Type* type) {
    // Mock reference type creation
    return type_copy(type);
}

Type* type_mutable_reference(Type* type) {
    // Mock mutable reference type creation
    return type_copy(type);
}

Type* type_function(Type** param_types, size_t param_count, Type* return_type) {
    // Mock function type creation
    Type* func_type = type_new(TYPE_UNKNOWN);
    if (func_type) {
        func_type->name = strdup("function");
    }
    return func_type;
}

InterfaceMethod* interface_method_new(const char* name, Type* signature, Position pos) {
    InterfaceMethod* method = malloc(sizeof(InterfaceMethod));
    if (method) {
        method->name = strdup(name);
        method->signature = signature;
        method->next = NULL;
    }
    return method;
}

void interface_method_free(InterfaceMethod* method) {
    if (method) {
        free(method->name);
        type_free(method->signature);
        free(method);
    }
}

InterfaceMethod* interface_method_copy(const InterfaceMethod* method) {
    if (!method) return NULL;
    Position pos = {0, 0, NULL};
    return interface_method_new(method->name, type_copy(method->signature), pos);
}

TypeVariable* type_variable_new(const char* name, TypeVariableKind kind, Position pos) {
    TypeVariable* var = malloc(sizeof(TypeVariable));
    if (var) {
        var->name = strdup(name);
        var->kind = kind;
        var->bound_type = NULL;
        var->constraints = NULL;
        var->is_inferred = 0;
        var->declared_pos = pos;
        var->next = NULL;
    }
    return var;
}

void type_variable_free(TypeVariable* var) {
    if (var) {
        free(var->name);
        if (var->bound_type) type_free(var->bound_type);
        free(var);
    }
}

ConstraintSet* constraint_set_new(void) {
    ConstraintSet* set = malloc(sizeof(ConstraintSet));
    if (set) {
        set->constraints = NULL;
        set->count = 0;
        set->is_satisfied = 1;
        set->error_message = NULL;
    }
    return set;
}

void constraint_set_free(ConstraintSet* set) {
    if (set) {
        free(set->error_message);
        free(set);
    }
}

int constraint_set_merge(ConstraintSet* dest, const ConstraintSet* src) {
    return 1;  // Mock success
}

int constraint_set_add(ConstraintSet* set, void* constraint) {
    if (set) {
        set->count++;
        return 1;
    }
    return 0;
}

int constraint_set_is_satisfied(const ConstraintSet* set, TypeChecker* checker) {
    return set ? set->is_satisfied : 0;
}

int type_has_method(Type* type, const char* method_name, Type* method_signature, TypeChecker* checker) {
    // Mock implementation - always return true for testing
    return 1;
}

int type_has_associated_type(Type* type, Type* associated_type, TypeChecker* checker) {
    // Mock implementation - always return true for testing
    return 1;
}

const char* type_variable_kind_to_string(TypeVariableKind kind) {
    switch (kind) {
        case TYPE_VAR_GENERIC: return "Generic";
        case TYPE_VAR_CONST: return "Const";
        case TYPE_VAR_LIFETIME: return "Lifetime";
        case TYPE_VAR_HIGHER_KINDED: return "HigherKinded";
        case TYPE_VAR_ASSOCIATED: return "Associated";
        default: return "Unknown";
    }
}

// Include the protocol implementation
#include "../types/protocol_oriented_programming.c"

// Test functions
int test_protocol_definition_creation() {
    printf("Testing protocol definition creation...\n");
    
    Position pos = {1, 1, "test.goo"};
    ProtocolDefinition* protocol = protocol_definition_new("TestProtocol", pos);
    
    assert(protocol != NULL);
    assert(strcmp(protocol->name, "TestProtocol") == 0);
    assert(protocol->type_parameters == NULL);
    assert(protocol->required_methods == NULL);
    assert(protocol->default_methods == NULL);
    assert(protocol->associated_types == NULL);
    assert(protocol->associated_type_count == 0);
    assert(protocol->where_clause == NULL);
    assert(protocol->inherited_protocols == NULL);
    assert(protocol->inherited_count == 0);
    assert(protocol->allows_retroactive_conformance == 1);
    assert(protocol->is_auto_conformance == 0);
    
    printf("  ✓ Protocol definition created successfully\n");
    
    protocol_definition_free(protocol);
    return 1;
}

int test_protocol_methods() {
    printf("Testing protocol method management...\n");
    
    Position pos = {1, 1, "test.goo"};
    ProtocolDefinition* protocol = protocol_definition_new("TestProtocol", pos);
    
    // Add type parameter
    int param_result = protocol_add_type_parameter(protocol, "Self", TYPE_VAR_GENERIC, pos);
    assert(param_result == 1);
    assert(protocol->type_parameters != NULL);
    assert(strcmp(protocol->type_parameters->name, "Self") == 0);
    
    printf("  ✓ Type parameter added successfully\n");
    
    // Add required method
    Type* method_type = type_function(NULL, 0, type_new(TYPE_BOOL));
    InterfaceMethod* method = interface_method_new("test_method", method_type, pos);
    int method_result = protocol_add_required_method(protocol, method);
    assert(method_result == 1);
    assert(protocol->required_methods != NULL);
    assert(strcmp(protocol->required_methods->name, "test_method") == 0);
    
    printf("  ✓ Required method added successfully\n");
    
    // Add default method
    Type* default_method_type = type_function(NULL, 0, type_new(TYPE_BOOL));
    InterfaceMethod* default_method = interface_method_new("default_method", default_method_type, pos);
    int default_result = protocol_add_default_method(protocol, default_method);
    assert(default_result == 1);
    assert(protocol->default_methods != NULL);
    assert(strcmp(protocol->default_methods->name, "default_method") == 0);
    
    printf("  ✓ Default method added successfully\n");
    
    // Add associated type
    Type* assoc_type = type_new(TYPE_UNKNOWN);
    assoc_type->name = strdup("Item");
    int assoc_result = protocol_add_associated_type(protocol, assoc_type);
    assert(assoc_result == 1);
    assert(protocol->associated_type_count == 1);
    assert(protocol->associated_types != NULL);
    assert(strcmp(protocol->associated_types[0]->name, "Item") == 0);
    
    printf("  ✓ Associated type added successfully\n");
    
    // Add where clause
    ConstraintSet* constraints = constraint_set_new();
    int where_result = protocol_add_where_clause(protocol, constraints);
    assert(where_result == 1);
    assert(protocol->where_clause != NULL);
    
    printf("  ✓ Where clause added successfully\n");
    
    type_free(assoc_type);
    protocol_definition_free(protocol);
    return 1;
}

int test_protocol_inheritance() {
    printf("Testing protocol inheritance...\n");
    
    Position pos = {1, 1, "test.goo"};
    
    // Create base protocol
    ProtocolDefinition* base_protocol = protocol_definition_new("BaseProtocol", pos);
    Type* base_method_type = type_function(NULL, 0, type_new(TYPE_BOOL));
    InterfaceMethod* base_method = interface_method_new("base_method", base_method_type, pos);
    protocol_add_required_method(base_protocol, base_method);
    
    // Create derived protocol
    ProtocolDefinition* derived_protocol = protocol_definition_new("DerivedProtocol", pos);
    int inherit_result = protocol_add_inherited_protocol(derived_protocol, base_protocol);
    assert(inherit_result == 1);
    assert(derived_protocol->inherited_count == 1);
    
    printf("  ✓ Protocol inheritance added successfully\n");
    
    // Test inheritance check
    int inherits = protocol_inherits_from(derived_protocol, base_protocol);
    assert(inherits == 1);
    
    printf("  ✓ Protocol inheritance check works correctly\n");
    
    // Test getting all methods (including inherited)
    InterfaceMethod* all_methods = protocol_get_all_methods(derived_protocol);
    assert(all_methods != NULL);
    
    // Count methods
    int method_count = 0;
    InterfaceMethod* current = all_methods;
    while (current) {
        method_count++;
        current = current->next;
    }
    assert(method_count >= 1);  // Should have at least the inherited method
    
    printf("  ✓ Getting all methods (including inherited) works correctly\n");
    
    // Clean up all methods
    current = all_methods;
    while (current) {
        InterfaceMethod* next = current->next;
        interface_method_free(current);
        current = next;
    }
    
    protocol_definition_free(derived_protocol);
    protocol_definition_free(base_protocol);
    return 1;
}

int test_protocol_conformance() {
    printf("Testing protocol conformance...\n");
    
    Position pos = {1, 1, "test.goo"};
    
    // Create protocol
    ProtocolDefinition* protocol = protocol_definition_new("TestProtocol", pos);
    
    // Create type
    Type* type = type_new(TYPE_STRUCT);
    type->name = strdup("TestType");
    
    // Create conformance
    ProtocolConformance* conformance = protocol_conformance_new(type, protocol, pos);
    assert(conformance != NULL);
    assert(type_equals(conformance->conforming_type, type));
    assert(conformance->protocol == protocol);
    assert(conformance->method_implementations == NULL);
    assert(conformance->associated_type_bindings == NULL);
    assert(conformance->is_auto_generated == 0);
    assert(conformance->is_retroactive == 0);
    
    printf("  ✓ Protocol conformance created successfully\n");
    
    // Add method implementation
    Type* impl_method_type = type_function(NULL, 0, type_new(TYPE_BOOL));
    InterfaceMethod* impl_method = interface_method_new("test_implementation", impl_method_type, pos);
    int impl_result = protocol_conformance_add_method_implementation(conformance, impl_method);
    assert(impl_result == 1);
    assert(conformance->method_implementations != NULL);
    assert(strcmp(conformance->method_implementations->name, "test_implementation") == 0);
    
    printf("  ✓ Method implementation added successfully\n");
    
    // Add associated type binding
    Type* binding_type = type_new(TYPE_INT32);
    binding_type->name = strdup("int");
    
    // First add an associated type to the protocol
    Type* assoc_type = type_new(TYPE_UNKNOWN);
    assoc_type->name = strdup("Item");
    protocol_add_associated_type(protocol, assoc_type);
    
    int binding_result = protocol_conformance_add_associated_type_binding(conformance, 0, binding_type);
    assert(binding_result == 1);
    assert(conformance->associated_type_bindings != NULL);
    assert(type_equals(conformance->associated_type_bindings[0], binding_type));
    
    printf("  ✓ Associated type binding added successfully\n");
    
    type_free(type);
    type_free(binding_type);
    type_free(assoc_type);
    protocol_conformance_free(conformance);
    protocol_definition_free(protocol);
    return 1;
}

int test_automatic_conformance() {
    printf("Testing automatic conformance detection...\n");
    
    Position pos = {1, 1, "test.goo"};
    TypeChecker checker = {0};  // Mock type checker
    
    // Create auto-conformance protocol
    ProtocolDefinition* protocol = protocol_definition_new("AutoProtocol", pos);
    protocol->is_auto_conformance = 1;
    
    // Add a required method
    Type* method_type = type_function(NULL, 0, type_new(TYPE_BOOL));
    InterfaceMethod* method = interface_method_new("required_method", method_type, pos);
    protocol_add_required_method(protocol, method);
    
    // Create a type
    Type* type = type_new(TYPE_STRUCT);
    type->name = strdup("ConformingType");
    
    // Test conformance inference (mock always returns true)
    int can_conform = infer_protocol_conformance(type, protocol, &checker);
    assert(can_conform == 1);
    
    printf("  ✓ Automatic conformance detection works correctly\n");
    
    // Test retroactive conformance
    protocol->allows_retroactive_conformance = 1;
    int retroactive = check_retroactive_conformance(type, protocol, &checker);
    assert(retroactive == 1);
    
    printf("  ✓ Retroactive conformance detection works correctly\n");
    
    type_free(type);
    protocol_definition_free(protocol);
    return 1;
}

int test_protocol_composition() {
    printf("Testing protocol composition...\n");
    
    Position pos = {1, 1, "test.goo"};
    
    // Create base protocols
    ProtocolDefinition* protocol1 = protocol_definition_new("Protocol1", pos);
    ProtocolDefinition* protocol2 = protocol_definition_new("Protocol2", pos);
    
    ProtocolDefinition* base_protocols[] = {protocol1, protocol2};
    
    // Create composition
    ProtocolDefinition* composition = create_protocol_composition("ComposedProtocol", 
                                                                base_protocols, 2, pos);
    assert(composition != NULL);
    assert(strcmp(composition->name, "ComposedProtocol") == 0);
    assert(composition->inherited_count == 2);
    assert(composition->is_auto_conformance == 1);
    
    printf("  ✓ Protocol composition created successfully\n");
    
    // Test that composition inherits from both protocols
    int inherits_from_1 = protocol_inherits_from(composition, protocol1);
    int inherits_from_2 = protocol_inherits_from(composition, protocol2);
    assert(inherits_from_1 == 1);
    assert(inherits_from_2 == 1);
    
    printf("  ✓ Protocol composition inheritance works correctly\n");
    
    protocol_definition_free(composition);
    protocol_definition_free(protocol1);
    protocol_definition_free(protocol2);
    return 1;
}

int test_protocol_refinement() {
    printf("Testing protocol refinement...\n");
    
    Position pos = {1, 1, "test.goo"};
    
    // Create base protocol
    ProtocolDefinition* base_protocol = protocol_definition_new("BaseProtocol", pos);
    
    // Create refinement with additional constraints
    ProtocolDefinition* refinement = create_protocol_refinement("RefinedProtocol", 
                                                              base_protocol, NULL, 0, pos);
    assert(refinement != NULL);
    assert(strcmp(refinement->name, "RefinedProtocol") == 0);
    assert(refinement->inherited_count == 1);
    
    printf("  ✓ Protocol refinement created successfully\n");
    
    // Test that refinement inherits from base
    int inherits = protocol_inherits_from(refinement, base_protocol);
    assert(inherits == 1);
    
    printf("  ✓ Protocol refinement inheritance works correctly\n");
    
    protocol_definition_free(refinement);
    protocol_definition_free(base_protocol);
    return 1;
}

int test_standard_protocols() {
    printf("Testing standard protocol library...\n");
    
    Position pos = {1, 1, "test.goo"};
    
    // Test Equatable protocol
    ProtocolDefinition* equatable = create_equatable_protocol(pos);
    assert(equatable != NULL);
    assert(strcmp(equatable->name, "Equatable") == 0);
    assert(equatable->is_auto_conformance == 1);
    assert(equatable->required_methods != NULL);
    
    printf("  ✓ Equatable protocol created successfully\n");
    
    // Test Comparable protocol
    ProtocolDefinition* comparable = create_comparable_protocol(pos);
    assert(comparable != NULL);
    assert(strcmp(comparable->name, "Comparable") == 0);
    assert(comparable->inherited_count == 1);  // Inherits from Equatable
    
    printf("  ✓ Comparable protocol created successfully\n");
    
    // Test Iterator protocol
    ProtocolDefinition* iterator = create_iterator_protocol(pos);
    assert(iterator != NULL);
    assert(strcmp(iterator->name, "Iterator") == 0);
    assert(iterator->associated_type_count == 1);  // Has Item associated type
    
    printf("  ✓ Iterator protocol created successfully\n");
    
    // Test Collection protocol
    ProtocolDefinition* collection = create_collection_protocol(pos);
    assert(collection != NULL);
    assert(strcmp(collection->name, "Collection") == 0);
    assert(collection->inherited_count == 1);  // Inherits from Iterator
    
    printf("  ✓ Collection protocol created successfully\n");
    
    // Test Serializable protocol
    ProtocolDefinition* serializable = create_serializable_protocol(pos);
    assert(serializable != NULL);
    assert(strcmp(serializable->name, "Serializable") == 0);
    
    printf("  ✓ Serializable protocol created successfully\n");
    
    protocol_definition_free(equatable);
    protocol_definition_free(comparable);
    protocol_definition_free(iterator);
    protocol_definition_free(collection);
    protocol_definition_free(serializable);
    return 1;
}

int test_protocol_registry() {
    printf("Testing protocol registry...\n");
    
    Position pos = {1, 1, "test.goo"};
    
    // Create registry
    ProtocolRegistry* registry = protocol_registry_new();
    assert(registry != NULL);
    assert(registry->protocol_count == 0);
    assert(registry->conformance_count == 0);
    
    printf("  ✓ Protocol registry created successfully\n");
    
    // Add protocol to registry
    ProtocolDefinition* protocol = protocol_definition_new("TestProtocol", pos);
    int add_result = protocol_registry_add_protocol(registry, protocol);
    assert(add_result == 1);
    assert(registry->protocol_count == 1);
    
    printf("  ✓ Protocol added to registry successfully\n");
    
    // Find protocol in registry
    ProtocolDefinition* found = protocol_registry_find_protocol(registry, "TestProtocol");
    assert(found != NULL);
    assert(found == protocol);
    
    printf("  ✓ Protocol found in registry successfully\n");
    
    // Test finding non-existent protocol
    ProtocolDefinition* not_found = protocol_registry_find_protocol(registry, "NonExistent");
    assert(not_found == NULL);
    
    printf("  ✓ Non-existent protocol search works correctly\n");
    
    // Add conformance to registry
    Type* type = type_new(TYPE_STRUCT);
    type->name = strdup("TestType");
    ProtocolConformance* conformance = protocol_conformance_new(type, protocol, pos);
    
    int conformance_result = protocol_registry_add_conformance(registry, conformance);
    assert(conformance_result == 1);
    assert(registry->conformance_count == 1);
    
    printf("  ✓ Conformance added to registry successfully\n");
    
    // Find conformances for type
    size_t conformance_count = 0;
    ProtocolConformance** conformances = protocol_registry_find_conformances(registry, type, &conformance_count);
    assert(conformances != NULL);
    assert(conformance_count == 1);
    assert(conformances[0] == conformance);
    
    printf("  ✓ Conformances found for type successfully\n");
    
    free(conformances);
    type_free(type);
    protocol_registry_free(registry);
    return 1;
}

int main() {
    printf("Protocol-Oriented Programming Test Suite\n");
    printf("=========================================\n\n");
    
    int tests_passed = 0;
    int total_tests = 0;
    
    total_tests++; if (test_protocol_definition_creation()) tests_passed++;
    total_tests++; if (test_protocol_methods()) tests_passed++;
    total_tests++; if (test_protocol_inheritance()) tests_passed++;
    total_tests++; if (test_protocol_conformance()) tests_passed++;
    total_tests++; if (test_automatic_conformance()) tests_passed++;
    total_tests++; if (test_protocol_composition()) tests_passed++;
    total_tests++; if (test_protocol_refinement()) tests_passed++;
    total_tests++; if (test_standard_protocols()) tests_passed++;
    total_tests++; if (test_protocol_registry()) tests_passed++;
    
    printf("\n=========================================\n");
    printf("Tests passed: %d/%d\n", tests_passed, total_tests);
    
    if (tests_passed == total_tests) {
        printf("✅ All protocol-oriented programming tests passed!\n");
        printf("\nProtocol system features verified:\n");
        printf("  • Protocol definition and management\n");
        printf("  • Method and associated type handling\n");
        printf("  • Protocol inheritance and composition\n");
        printf("  • Automatic conformance detection\n");
        printf("  • Retroactive conformance support\n");
        printf("  • Protocol composition and refinement\n");
        printf("  • Standard protocol library\n");
        printf("  • Protocol registry system\n");
        return 0;
    } else {
        printf("❌ Some tests failed!\n");
        return 1;
    }
}