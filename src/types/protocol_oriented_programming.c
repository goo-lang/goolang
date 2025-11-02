#include "types/constraint_inference.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// =============================================================================
// 22.5: Protocol-Oriented Programming System Implementation
// =============================================================================

// Helper function for string duplication
static char* str_dup(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char* dup = malloc(len + 1);
    if (dup) {
        strcpy(dup, str);
    }
    return dup;
}

// =============================================================================
// Protocol Definition Management
// =============================================================================

ProtocolDefinition* protocol_definition_new(const char* name, Position pos) {
    ProtocolDefinition* protocol = malloc(sizeof(ProtocolDefinition));
    if (!protocol) return NULL;
    
    protocol->name = str_dup(name);
    protocol->type_parameters = NULL;
    protocol->required_methods = NULL;
    protocol->default_methods = NULL;
    protocol->associated_types = NULL;
    protocol->associated_type_count = 0;
    protocol->where_clause = NULL;
    protocol->inherited_protocols = NULL;
    protocol->inherited_count = 0;
    protocol->allows_retroactive_conformance = 1; // Default to allowing retroactive conformance
    protocol->is_auto_conformance = 0; // Default to explicit conformance
    protocol->defined_pos = pos;
    protocol->next = NULL;
    
    return protocol;
}

void protocol_definition_free(ProtocolDefinition* protocol) {
    if (!protocol) return;
    
    free(protocol->name);
    
    // Free type parameters
    TypeVariable* param = protocol->type_parameters;
    while (param) {
        TypeVariable* next = param->next;
        type_variable_free(param);
        param = next;
    }
    
    // Free required methods
    InterfaceMethod* method = protocol->required_methods;
    while (method) {
        InterfaceMethod* next = method->next;
        interface_method_free(method);
        method = next;
    }
    
    // Free default methods
    method = protocol->default_methods;
    while (method) {
        InterfaceMethod* next = method->next;
        interface_method_free(method);
        method = next;
    }
    
    // Free associated types
    if (protocol->associated_types) {
        for (size_t i = 0; i < protocol->associated_type_count; i++) {
            if (protocol->associated_types[i]) {
                type_free(protocol->associated_types[i]);
            }
        }
        free(protocol->associated_types);
    }
    
    // Free where clause
    if (protocol->where_clause) {
        constraint_set_free(protocol->where_clause);
    }
    
    // Free inherited protocols
    if (protocol->inherited_protocols) {
        for (size_t i = 0; i < protocol->inherited_count; i++) {
            protocol_definition_free(&protocol->inherited_protocols[i]);
        }
        free(protocol->inherited_protocols);
    }
    
    free(protocol);
}

int protocol_add_required_method(ProtocolDefinition* protocol, InterfaceMethod* method) {
    if (!protocol || !method) return 0;
    
    method->next = protocol->required_methods;
    protocol->required_methods = method;
    
    return 1;
}

int protocol_add_default_method(ProtocolDefinition* protocol, InterfaceMethod* method) {
    if (!protocol || !method) return 0;
    
    method->next = protocol->default_methods;
    protocol->default_methods = method;
    
    return 1;
}

int protocol_add_associated_type(ProtocolDefinition* protocol, Type* associated_type) {
    if (!protocol || !associated_type) return 0;
    
    Type** new_types = realloc(protocol->associated_types, 
                              sizeof(Type*) * (protocol->associated_type_count + 1));
    if (!new_types) return 0;
    
    protocol->associated_types = new_types;
    protocol->associated_types[protocol->associated_type_count] = type_copy(associated_type);
    protocol->associated_type_count++;
    
    return 1;
}

int protocol_add_type_parameter(ProtocolDefinition* protocol, const char* param_name, 
                               TypeVariableKind kind, Position pos) {
    if (!protocol || !param_name) return 0;
    
    TypeVariable* param = type_variable_new(param_name, kind, pos);
    if (!param) return 0;
    
    param->next = protocol->type_parameters;
    protocol->type_parameters = param;
    
    return 1;
}

int protocol_add_where_clause(ProtocolDefinition* protocol, ConstraintSet* constraints) {
    if (!protocol || !constraints) return 0;
    
    if (protocol->where_clause) {
        // Merge with existing constraints
        constraint_set_merge(protocol->where_clause, constraints);
        constraint_set_free(constraints);
    } else {
        protocol->where_clause = constraints;
    }
    
    return 1;
}

// =============================================================================
// Protocol Inheritance and Composition
// =============================================================================

int protocol_add_inherited_protocol(ProtocolDefinition* protocol, ProtocolDefinition* inherited) {
    if (!protocol || !inherited) return 0;
    
    ProtocolDefinition* new_inherited = realloc(protocol->inherited_protocols,
                                               sizeof(ProtocolDefinition) * (protocol->inherited_count + 1));
    if (!new_inherited) return 0;
    
    protocol->inherited_protocols = new_inherited;
    protocol->inherited_protocols[protocol->inherited_count] = *inherited; // Copy the protocol
    protocol->inherited_count++;
    
    return 1;
}

// Check if a protocol inherits from another protocol (directly or indirectly)
int protocol_inherits_from(ProtocolDefinition* protocol, ProtocolDefinition* ancestor) {
    if (!protocol || !ancestor) return 0;
    
    // Check direct inheritance
    for (size_t i = 0; i < protocol->inherited_count; i++) {
        if (strcmp(protocol->inherited_protocols[i].name, ancestor->name) == 0) {
            return 1;
        }
        
        // Check transitive inheritance
        if (protocol_inherits_from(&protocol->inherited_protocols[i], ancestor)) {
            return 1;
        }
    }
    
    return 0;
}

// Get all methods (required + default + inherited) for a protocol
InterfaceMethod* protocol_get_all_methods(ProtocolDefinition* protocol) {
    if (!protocol) return NULL;
    
    InterfaceMethod* all_methods = NULL;
    InterfaceMethod* last_method = NULL;
    
    // Add required methods
    InterfaceMethod* method = protocol->required_methods;
    while (method) {
        InterfaceMethod* copy = interface_method_copy(method);
        if (copy) {
            if (last_method) {
                last_method->next = copy;
            } else {
                all_methods = copy;
            }
            last_method = copy;
        }
        method = method->next;
    }
    
    // Add default methods
    method = protocol->default_methods;
    while (method) {
        InterfaceMethod* copy = interface_method_copy(method);
        if (copy) {
            if (last_method) {
                last_method->next = copy;
            } else {
                all_methods = copy;
            }
            last_method = copy;
        }
        method = method->next;
    }
    
    // Add inherited methods
    for (size_t i = 0; i < protocol->inherited_count; i++) {
        InterfaceMethod* inherited_methods = protocol_get_all_methods(&protocol->inherited_protocols[i]);
        InterfaceMethod* inherited_method = inherited_methods;
        
        while (inherited_method) {
            InterfaceMethod* copy = interface_method_copy(inherited_method);
            if (copy) {
                if (last_method) {
                    last_method->next = copy;
                } else {
                    all_methods = copy;
                }
                last_method = copy;
            }
            inherited_method = inherited_method->next;
        }
        
        // Free the temporary list
        method = inherited_methods;
        while (method) {
            InterfaceMethod* next = method->next;
            interface_method_free(method);
            method = next;
        }
    }
    
    return all_methods;
}

// =============================================================================
// Protocol Conformance Management
// =============================================================================

ProtocolConformance* protocol_conformance_new(Type* type, ProtocolDefinition* protocol, Position pos) {
    if (!type || !protocol) return NULL;
    
    ProtocolConformance* conformance = malloc(sizeof(ProtocolConformance));
    if (!conformance) return NULL;
    
    conformance->conforming_type = type_copy(type);
    conformance->protocol = protocol;
    conformance->method_implementations = NULL;
    conformance->associated_type_bindings = NULL;
    conformance->is_auto_generated = 0;
    conformance->is_retroactive = 0;
    conformance->conformance_pos = pos;
    
    return conformance;
}

void protocol_conformance_free(ProtocolConformance* conformance) {
    if (!conformance) return;
    
    if (conformance->conforming_type) {
        type_free(conformance->conforming_type);
    }
    
    // Free method implementations
    InterfaceMethod* method = conformance->method_implementations;
    while (method) {
        InterfaceMethod* next = method->next;
        interface_method_free(method);
        method = next;
    }
    
    // Free associated type bindings
    if (conformance->associated_type_bindings) {
        for (size_t i = 0; i < conformance->protocol->associated_type_count; i++) {
            if (conformance->associated_type_bindings[i]) {
                type_free(conformance->associated_type_bindings[i]);
            }
        }
        free(conformance->associated_type_bindings);
    }
    
    free(conformance);
}

int protocol_conformance_add_method_implementation(ProtocolConformance* conformance, 
                                                  InterfaceMethod* implementation) {
    if (!conformance || !implementation) return 0;
    
    implementation->next = conformance->method_implementations;
    conformance->method_implementations = implementation;
    
    return 1;
}

int protocol_conformance_add_associated_type_binding(ProtocolConformance* conformance, 
                                                    size_t index, Type* binding) {
    if (!conformance || !binding || index >= conformance->protocol->associated_type_count) return 0;
    
    if (!conformance->associated_type_bindings) {
        conformance->associated_type_bindings = malloc(sizeof(Type*) * conformance->protocol->associated_type_count);
        if (!conformance->associated_type_bindings) return 0;
        memset(conformance->associated_type_bindings, 0, sizeof(Type*) * conformance->protocol->associated_type_count);
    }
    
    if (conformance->associated_type_bindings[index]) {
        type_free(conformance->associated_type_bindings[index]);
    }
    
    conformance->associated_type_bindings[index] = type_copy(binding);
    return 1;
}

// =============================================================================
// Protocol Registry Management
// =============================================================================

ProtocolRegistry* protocol_registry_new(void) {
    ProtocolRegistry* registry = malloc(sizeof(ProtocolRegistry));
    if (!registry) return NULL;
    
    registry->protocols = NULL;
    registry->protocol_count = 0;
    registry->protocol_capacity = 0;
    registry->conformances = NULL;
    registry->conformance_count = 0;
    registry->conformance_capacity = 0;
    
    return registry;
}

void protocol_registry_free(ProtocolRegistry* registry) {
    if (!registry) return;
    
    // Free protocols
    if (registry->protocols) {
        for (size_t i = 0; i < registry->protocol_count; i++) {
            protocol_definition_free(registry->protocols[i]);
        }
        free(registry->protocols);
    }
    
    // Free conformances
    if (registry->conformances) {
        for (size_t i = 0; i < registry->conformance_count; i++) {
            protocol_conformance_free(registry->conformances[i]);
        }
        free(registry->conformances);
    }
    
    free(registry);
}

int protocol_registry_add_protocol(ProtocolRegistry* registry, ProtocolDefinition* protocol) {
    if (!registry || !protocol) return 0;
    
    if (registry->protocol_count >= registry->protocol_capacity) {
        size_t new_capacity = registry->protocol_capacity == 0 ? 8 : registry->protocol_capacity * 2;
        ProtocolDefinition** new_protocols = realloc(registry->protocols, 
                                                     sizeof(ProtocolDefinition*) * new_capacity);
        if (!new_protocols) return 0;
        
        registry->protocols = new_protocols;
        registry->protocol_capacity = new_capacity;
    }
    
    registry->protocols[registry->protocol_count] = protocol;
    registry->protocol_count++;
    
    return 1;
}

int protocol_registry_add_conformance(ProtocolRegistry* registry, ProtocolConformance* conformance) {
    if (!registry || !conformance) return 0;
    
    if (registry->conformance_count >= registry->conformance_capacity) {
        size_t new_capacity = registry->conformance_capacity == 0 ? 8 : registry->conformance_capacity * 2;
        ProtocolConformance** new_conformances = realloc(registry->conformances, 
                                                         sizeof(ProtocolConformance*) * new_capacity);
        if (!new_conformances) return 0;
        
        registry->conformances = new_conformances;
        registry->conformance_capacity = new_capacity;
    }
    
    registry->conformances[registry->conformance_count] = conformance;
    registry->conformance_count++;
    
    return 1;
}

ProtocolDefinition* protocol_registry_find_protocol(ProtocolRegistry* registry, const char* name) {
    if (!registry || !name) return NULL;
    
    for (size_t i = 0; i < registry->protocol_count; i++) {
        if (registry->protocols[i]->name && strcmp(registry->protocols[i]->name, name) == 0) {
            return registry->protocols[i];
        }
    }
    
    return NULL;
}

ProtocolConformance* protocol_registry_find_conformance(ProtocolRegistry* registry, 
                                                       Type* type, ProtocolDefinition* protocol) {
    if (!registry || !type || !protocol) return NULL;
    
    for (size_t i = 0; i < registry->conformance_count; i++) {
        ProtocolConformance* conformance = registry->conformances[i];
        if (conformance->protocol == protocol && type_equals(conformance->conforming_type, type)) {
            return conformance;
        }
    }
    
    return NULL;
}

// =============================================================================
// Retroactive Conformance Support
// =============================================================================

// Add conformance for an existing type to an existing protocol
int protocol_add_retroactive_conformance(ProtocolRegistry* registry, Type* type, 
                                        ProtocolDefinition* protocol, Position pos) {
    if (!registry || !type || !protocol) return 0;
    
    // Check if protocol allows retroactive conformance
    if (!protocol->allows_retroactive_conformance) {
        return 0;
    }
    
    // Check if conformance already exists
    if (protocol_registry_find_conformance(registry, type, protocol)) {
        return 0; // Already conforms
    }
    
    // Create new retroactive conformance
    ProtocolConformance* conformance = protocol_conformance_new(type, protocol, pos);
    if (!conformance) return 0;
    
    conformance->is_retroactive = 1;
    
    // Add to registry
    return protocol_registry_add_conformance(registry, conformance);
}

// Check if a type can retroactively conform to a protocol
int can_add_retroactive_conformance(Type* type, ProtocolDefinition* protocol, TypeChecker* checker) {
    if (!type || !protocol || !checker) return 0;
    
    // Must allow retroactive conformance
    if (!protocol->allows_retroactive_conformance) return 0;
    
    // Type must have all required methods
    return type_has_required_methods(type, protocol, checker);
}

// =============================================================================
// Automatic Conformance Inference
// =============================================================================

// Automatically infer protocol conformances for a type
int infer_protocol_conformances(ProtocolRegistry* registry, Type* type, TypeChecker* checker) {
    if (!registry || !type || !checker) return 0;
    
    int conformances_added = 0;
    
    for (size_t i = 0; i < registry->protocol_count; i++) {
        ProtocolDefinition* protocol = registry->protocols[i];
        
        // Skip protocols that don't allow auto-conformance
        if (!protocol->is_auto_conformance) continue;
        
        // Skip if already conforms
        if (protocol_registry_find_conformance(registry, type, protocol)) continue;
        
        // Check if type satisfies protocol requirements
        if (type_has_required_methods(type, protocol, checker)) {
            Position pos = {0, 0, 0, ""}; // Auto-inferred position
            ProtocolConformance* conformance = protocol_conformance_new(type, protocol, pos);
            if (conformance) {
                conformance->is_auto_generated = 1;
                if (protocol_registry_add_conformance(registry, conformance)) {
                    conformances_added++;
                }
            }
        }
    }
    
    return conformances_added;
}

// Check if a type automatically conforms to a protocol
int type_auto_conforms_to_protocol(Type* type, ProtocolDefinition* protocol, TypeChecker* checker) {
    if (!type || !protocol || !checker) return 0;
    
    // Protocol must allow auto-conformance
    if (!protocol->is_auto_conformance) return 0;
    
    // Type must satisfy all requirements
    return type_satisfies_protocol_requirements(type, protocol, checker);
}

// =============================================================================
// Protocol Extensions and Default Implementations
// =============================================================================

typedef struct ProtocolExtension {
    ProtocolDefinition* extended_protocol;   // Protocol being extended
    InterfaceMethod* extension_methods;      // Additional methods provided
    ConstraintSet* where_clause;             // Where clause for conditional extensions
    Type* conforming_type;                   // Specific type (NULL for all conforming types)
    int is_conditional;                      // Whether this is a conditional extension
    Position defined_pos;                    // Where this extension was defined
    struct ProtocolExtension* next;          // For linked lists
} ProtocolExtension;

ProtocolExtension* protocol_extension_new(ProtocolDefinition* protocol, Position pos) {
    if (!protocol) return NULL;
    
    ProtocolExtension* extension = malloc(sizeof(ProtocolExtension));
    if (!extension) return NULL;
    
    extension->extended_protocol = protocol;
    extension->extension_methods = NULL;
    extension->where_clause = NULL;
    extension->conforming_type = NULL;
    extension->is_conditional = 0;
    extension->defined_pos = pos;
    extension->next = NULL;
    
    return extension;
}

void protocol_extension_free(ProtocolExtension* extension) {
    if (!extension) return;
    
    // Free extension methods
    InterfaceMethod* method = extension->extension_methods;
    while (method) {
        InterfaceMethod* next = method->next;
        interface_method_free(method);
        method = next;
    }
    
    if (extension->where_clause) {
        constraint_set_free(extension->where_clause);
    }
    
    if (extension->conforming_type) {
        type_free(extension->conforming_type);
    }
    
    free(extension);
}

int protocol_extension_add_method(ProtocolExtension* extension, InterfaceMethod* method) {
    if (!extension || !method) return 0;
    
    method->next = extension->extension_methods;
    extension->extension_methods = method;
    
    return 1;
}

int protocol_extension_add_where_clause(ProtocolExtension* extension, ConstraintSet* constraints) {
    if (!extension || !constraints) return 0;
    
    if (extension->where_clause) {
        constraint_set_merge(extension->where_clause, constraints);
        constraint_set_free(constraints);
    } else {
        extension->where_clause = constraints;
    }
    
    extension->is_conditional = 1;
    return 1;
}

// Check if a protocol extension applies to a specific type
int protocol_extension_applies_to_type(ProtocolExtension* extension, Type* type, TypeChecker* checker) {
    if (!extension || !type || !checker) return 0;
    
    // If extension is for a specific type, check exact match
    if (extension->conforming_type) {
        return type_equals(extension->conforming_type, type);
    }
    
    // If conditional extension, check where clause
    if (extension->is_conditional && extension->where_clause) {
        return constraint_set_is_satisfied_for_type(extension->where_clause, type, checker);
    }
    
    // Otherwise, applies to all conforming types
    return 1;
}

// =============================================================================
// Enhanced Protocol Conformance Checking
// =============================================================================

// Check if a type satisfies all protocol requirements
int type_satisfies_protocol_requirements(Type* type, ProtocolDefinition* protocol, TypeChecker* checker) {
    if (!type || !protocol || !checker) return 0;
    
    // Check required methods
    if (!type_has_required_methods(type, protocol, checker)) {
        return 0;
    }
    
    // Check associated type requirements
    if (!type_satisfies_associated_type_requirements(type, protocol, checker)) {
        return 0;
    }
    
    // Check where clause constraints
    if (protocol->where_clause && !constraint_set_is_satisfied_for_type(protocol->where_clause, type, checker)) {
        return 0;
    }
    
    // Check inherited protocol requirements
    for (size_t i = 0; i < protocol->inherited_count; i++) {
        if (!type_satisfies_protocol_requirements(type, &protocol->inherited_protocols[i], checker)) {
            return 0;
        }
    }
    
    return 1;
}

int type_satisfies_associated_type_requirements(Type* type, ProtocolDefinition* protocol, TypeChecker* checker) {
    if (!type || !protocol || !checker) return 0;
    
    // For now, assume all associated types are satisfied
    // In a full implementation, this would check that the type provides
    // implementations for all associated types
    return 1;
}

int constraint_set_is_satisfied_for_type(ConstraintSet* constraints, Type* type, TypeChecker* checker) {
    if (!constraints || !type || !checker) return 1; // No constraints = satisfied
    
    InterfaceConstraint* constraint = constraints->constraints;
    while (constraint) {
        if (!constraint_is_satisfied_for_type(constraint, type, checker)) {
            return 0;
        }
        constraint = constraint->next;
    }
    
    return 1;
}

int constraint_is_satisfied_for_type(InterfaceConstraint* constraint, Type* type, TypeChecker* checker) {
    if (!constraint || !type || !checker) return 1;
    
    switch (constraint->kind) {
        case CONSTRAINT_IMPLEMENTS:
            // Check if type implements the specified protocol/interface
            if (constraint->protocol_name) {
                // Look up the protocol and check conformance
                return 1; // Simplified for now
            }
            return 0;
            
        case CONSTRAINT_SIZE:
            // Check if type has known size at compile time
            return type_size(type) > 0;
            
        case CONSTRAINT_COPY:
            // Check if type can be copied (no unique ownership)
            return !type_has_unique_ownership(type);
            
        case CONSTRAINT_NUMERIC:
            // Check if type is numeric
            return type_is_numeric(type);
            
        case CONSTRAINT_ARITHMETIC:
            // Check if type supports arithmetic operations
            return type_supports_arithmetic(type);
            
        default:
            return 1; // Unknown constraints are assumed satisfied
    }
}

// Helper functions for constraint checking
int type_has_unique_ownership(Type* type) {
    if (!type) return 0;
    return type->kind == TYPE_POINTER || type->kind == TYPE_REFERENCE;
}

int type_supports_arithmetic(Type* type) {
    if (!type) return 0;
    return type_is_numeric(type);
}

// =============================================================================
// Automatic Conformance Detection
// =============================================================================

// Check if a type has all required methods for a protocol
int type_has_required_methods(Type* type, ProtocolDefinition* protocol, TypeChecker* checker) {
    if (!type || !protocol || !checker) return 0;
    
    InterfaceMethod* required_method = protocol->required_methods;
    while (required_method) {
        if (!type_has_method(type, required_method->name, required_method->type, checker)) {
            return 0;
        }
        required_method = required_method->next;
    }
    
    return 1;
}

// Check if a type has a specific method
int type_has_method(Type* type, const char* method_name, Type* method_signature, TypeChecker* checker) {
    if (!type || !method_name || !checker) return 0;
    
    switch (type->kind) {
        case TYPE_STRUCT: {
            // For structs, check if they have methods defined
            // This would typically involve looking up in the type checker's symbol table
            // For now, simplified implementation
            return 1; // Assume structs can have methods
        }
        
        case TYPE_INTERFACE: {
            // Check if interface has the method
            InterfaceMethod* method = type->data.interface.methods;
            while (method) {
                if (strcmp(method->name, method_name) == 0) {
                    if (!method_signature || type_compatible(method->type, method_signature)) {
                        return 1;
                    }
                }
                method = method->next;
            }
            return 0;
        }
        
        default:
            // For primitive types, check if they have built-in methods
            return type_has_builtin_method(type, method_name);
    }
}

// Check if a primitive type has a built-in method
int type_has_builtin_method(Type* type, const char* method_name) {
    if (!type || !method_name) return 0;
    
    switch (type->kind) {
        case TYPE_INT32:
        case TYPE_INT64:
        case TYPE_UINT32:
        case TYPE_UINT64:
            // Integer types have arithmetic methods
            return (strcmp(method_name, "add") == 0 ||
                   strcmp(method_name, "sub") == 0 ||
                   strcmp(method_name, "mul") == 0 ||
                   strcmp(method_name, "div") == 0);
                   
        case TYPE_FLOAT32:
        case TYPE_FLOAT64:
            // Float types have arithmetic methods
            return (strcmp(method_name, "add") == 0 ||
                   strcmp(method_name, "sub") == 0 ||
                   strcmp(method_name, "mul") == 0 ||
                   strcmp(method_name, "div") == 0);
                   
        case TYPE_STRING:
            // String types have string methods
            return (strcmp(method_name, "length") == 0 ||
                   strcmp(method_name, "concat") == 0 ||
                   strcmp(method_name, "substring") == 0);
                   
        default:
            return 0;
    }
}

// =============================================================================
// Standard Protocol Library
// =============================================================================

// Create built-in protocols that are commonly used
ProtocolDefinition* create_equatable_protocol(Position pos) {
    ProtocolDefinition* protocol = protocol_definition_new("Equatable", pos);
    if (!protocol) return NULL;
    
    // Add equals method
    InterfaceMethod* equals_method = interface_method_new("equals", NULL);
    if (equals_method) {
        // Create function type: (Self, Self) -> bool
        Type* bool_type = type_new(TYPE_BOOL);
        equals_method->type = bool_type;
        protocol_add_required_method(protocol, equals_method);
    }
    
    protocol->is_auto_conformance = 1; // Allow automatic conformance
    return protocol;
}

ProtocolDefinition* create_comparable_protocol(Position pos) {
    ProtocolDefinition* protocol = protocol_definition_new("Comparable", pos);
    if (!protocol) return NULL;
    
    // Add compare method
    InterfaceMethod* compare_method = interface_method_new("compare", NULL);
    if (compare_method) {
        // Create function type: (Self, Self) -> int
        Type* int_type = type_new(TYPE_INT32);
        compare_method->type = int_type;
        protocol_add_required_method(protocol, compare_method);
    }
    
    protocol->is_auto_conformance = 1; // Allow automatic conformance
    return protocol;
}

ProtocolDefinition* create_hashable_protocol(Position pos) {
    ProtocolDefinition* protocol = protocol_definition_new("Hashable", pos);
    if (!protocol) return NULL;
    
    // Add hash method
    InterfaceMethod* hash_method = interface_method_new("hash", NULL);
    if (hash_method) {
        // Create function type: (Self) -> usize
        Type* usize_type = type_new(TYPE_UINT64);
        hash_method->type = usize_type;
        protocol_add_required_method(protocol, hash_method);
    }
    
    protocol->is_auto_conformance = 1; // Allow automatic conformance
    return protocol;
}

ProtocolDefinition* create_displayable_protocol(Position pos) {
    ProtocolDefinition* protocol = protocol_definition_new("Displayable", pos);
    if (!protocol) return NULL;
    
    // Add to_string method
    InterfaceMethod* to_string_method = interface_method_new("to_string", NULL);
    if (to_string_method) {
        // Create function type: (Self) -> String
        Type* string_type = type_new(TYPE_STRING);
        to_string_method->type = string_type;
        protocol_add_required_method(protocol, to_string_method);
    }
    
    protocol->is_auto_conformance = 0; // Require explicit implementation
    return protocol;
}

ProtocolDefinition* create_cloneable_protocol(Position pos) {
    ProtocolDefinition* protocol = protocol_definition_new("Cloneable", pos);
    if (!protocol) return NULL;
    
    // Add clone method
    InterfaceMethod* clone_method = interface_method_new("clone", NULL);
    if (clone_method) {
        // Create function type: (Self) -> Self
        // For now, use a generic type
        Type* self_type = type_new(TYPE_UNKNOWN);
        clone_method->type = self_type;
        protocol_add_required_method(protocol, clone_method);
    }
    
    protocol->is_auto_conformance = 1; // Allow automatic conformance for simple types
    return protocol;
}

ProtocolDefinition* create_iterator_protocol(Position pos) {
    ProtocolDefinition* protocol = protocol_definition_new("Iterator", pos);
    if (!protocol) return NULL;
    
    // Add associated type for Item
    Type* item_type = type_new(TYPE_UNKNOWN);
    item_type->name = str_dup("Item");
    protocol_add_associated_type(protocol, item_type);
    
    // Add next method
    InterfaceMethod* next_method = interface_method_new("next", NULL);
    if (next_method) {
        // Create function type: (Self) -> Option<Item>
        Type* option_type = type_new(TYPE_UNKNOWN);
        option_type->name = str_dup("Option<Item>");
        next_method->type = option_type;
        protocol_add_required_method(protocol, next_method);
    }
    
    protocol->is_auto_conformance = 0; // Require explicit implementation
    return protocol;
}

// =============================================================================
// Helper Functions for Interface Methods
// =============================================================================

InterfaceMethod* interface_method_new(const char* name, Type* type) {
    InterfaceMethod* method = malloc(sizeof(InterfaceMethod));
    if (!method) return NULL;
    
    method->name = str_dup(name);
    method->type = type ? type_copy(type) : NULL;
    method->next = NULL;
    
    return method;
}

void interface_method_free(InterfaceMethod* method) {
    if (!method) return;
    
    free(method->name);
    if (method->type) {
        type_free(method->type);
    }
    free(method);
}

InterfaceMethod* interface_method_copy(const InterfaceMethod* method) {
    if (!method) return NULL;
    
    InterfaceMethod* copy = malloc(sizeof(InterfaceMethod));
    if (!copy) return NULL;
    
    copy->name = str_dup(method->name);
    copy->type = method->type ? type_copy(method->type) : NULL;
    copy->next = NULL;
    
    return copy;
}

// =============================================================================
// Protocol-Oriented Programming Utilities
// =============================================================================

// Initialize a protocol registry with standard protocols
int protocol_registry_init_standard_protocols(ProtocolRegistry* registry) {
    if (!registry) return 0;
    
    Position pos = {0, 0, 0, "built-in"};
    
    ProtocolDefinition* equatable = create_equatable_protocol(pos);
    ProtocolDefinition* comparable = create_comparable_protocol(pos);
    ProtocolDefinition* hashable = create_hashable_protocol(pos);
    ProtocolDefinition* displayable = create_displayable_protocol(pos);
    ProtocolDefinition* cloneable = create_cloneable_protocol(pos);
    ProtocolDefinition* iterator = create_iterator_protocol(pos);
    
    int success = 1;
    
    if (equatable) success &= protocol_registry_add_protocol(registry, equatable);
    if (comparable) success &= protocol_registry_add_protocol(registry, comparable);
    if (hashable) success &= protocol_registry_add_protocol(registry, hashable);
    if (displayable) success &= protocol_registry_add_protocol(registry, displayable);
    if (cloneable) success &= protocol_registry_add_protocol(registry, cloneable);
    if (iterator) success &= protocol_registry_add_protocol(registry, iterator);
    
    return success;
}

// Check if a type conforms to a protocol
int type_conforms_to_protocol(Type* type, ProtocolDefinition* protocol, 
                             ProtocolRegistry* registry, TypeChecker* checker) {
    if (!type || !protocol || !registry || !checker) return 0;
    
    // Check if there's an explicit conformance
    ProtocolConformance* conformance = protocol_registry_find_conformance(registry, type, protocol);
    if (conformance) return 1;
    
    // Check if type can auto-conform
    if (protocol->is_auto_conformance && type_satisfies_protocol_requirements(type, protocol, checker)) {
        return 1;
    }
    
    return 0;
}

// Get all protocols that a type conforms to
ProtocolDefinition** get_type_conforming_protocols(Type* type, ProtocolRegistry* registry, 
                                                  TypeChecker* checker, size_t* count) {
    if (!type || !registry || !checker || !count) return NULL;
    
    *count = 0;
    ProtocolDefinition** conforming_protocols = NULL;
    
    for (size_t i = 0; i < registry->protocol_count; i++) {
        ProtocolDefinition* protocol = registry->protocols[i];
        
        if (type_conforms_to_protocol(type, protocol, registry, checker)) {
            ProtocolDefinition** new_protocols = realloc(conforming_protocols, 
                                                         sizeof(ProtocolDefinition*) * (*count + 1));
            if (new_protocols) {
                conforming_protocols = new_protocols;
                conforming_protocols[*count] = protocol;
                (*count)++;
            }
        }
    }
    
    return conforming_protocols;
}

// Print protocol information for debugging
void print_protocol_definition(const ProtocolDefinition* protocol) {
    if (!protocol) {
        printf("Protocol: null\n");
        return;
    }
    
    printf("Protocol: %s\n", protocol->name ? protocol->name : "<unnamed>");
    printf("  Auto-conformance: %s\n", protocol->is_auto_conformance ? "yes" : "no");
    printf("  Retroactive conformance: %s\n", protocol->allows_retroactive_conformance ? "yes" : "no");
    
    if (protocol->required_methods) {
        printf("  Required methods:\n");
        InterfaceMethod* method = protocol->required_methods;
        while (method) {
            printf("    - %s\n", method->name ? method->name : "<unnamed>");
            method = method->next;
        }
    }
    
    if (protocol->default_methods) {
        printf("  Default methods:\n");
        InterfaceMethod* method = protocol->default_methods;
        while (method) {
            printf("    - %s (default)\n", method->name ? method->name : "<unnamed>");
            method = method->next;
        }
    }
    
    if (protocol->associated_type_count > 0) {
        printf("  Associated types: %zu\n", protocol->associated_type_count);
    }
    
    if (protocol->inherited_count > 0) {
        printf("  Inherited protocols: %zu\n", protocol->inherited_count);
    }
}
