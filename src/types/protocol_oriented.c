// Protocol-Oriented Programming System Implementation
// Part of Enhanced Interface System (Task #22.5)

#include "interface_system.h"
#include "types.h"
#include "ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// =============================================================================
// Protocol Definition Management
// =============================================================================

ProtocolDefinition* protocol_definition_new(const char* name, Position position) {
    ProtocolDefinition* protocol = calloc(1, sizeof(ProtocolDefinition));
    if (!protocol) return NULL;
    
    protocol->name = name ? strdup(name) : NULL;
    protocol->position = position;
    protocol->type_parameters = NULL;
    protocol->requirements = NULL;
    protocol->associated_types = NULL;
    protocol->super_protocols = NULL;
    protocol->default_implementations = NULL;
    protocol->requirement_count = 0;
    protocol->associated_type_count = 0;
    protocol->super_protocol_count = 0;
    protocol->default_impl_count = 0;
    protocol->is_object_safe = 1;  // Default to object-safe
    
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
    
    // Free requirements
    for (size_t i = 0; i < protocol->requirement_count; i++) {
        protocol_requirement_free(&protocol->requirements[i]);
    }
    free(protocol->requirements);
    
    // Free associated types
    for (size_t i = 0; i < protocol->associated_type_count; i++) {
        associated_type_free(&protocol->associated_types[i]);
    }
    free(protocol->associated_types);
    
    // Free super protocols
    for (size_t i = 0; i < protocol->super_protocol_count; i++) {
        if (protocol->super_protocols[i]) {
            // Note: Don't free the protocol itself, just the reference
        }
    }
    free(protocol->super_protocols);
    
    // Free default implementations
    for (size_t i = 0; i < protocol->default_impl_count; i++) {
        default_implementation_free(&protocol->default_implementations[i]);
    }
    free(protocol->default_implementations);
    
    free(protocol);
}

ProtocolDefinition* protocol_definition_copy(const ProtocolDefinition* protocol) {
    if (!protocol) return NULL;
    
    ProtocolDefinition* copy = protocol_definition_new(protocol->name, protocol->position);
    if (!copy) return NULL;
    
    // Copy type parameters
    TypeVariable* param = protocol->type_parameters;
    TypeVariable* last_copy = NULL;
    while (param) {
        TypeVariable* param_copy = type_variable_copy(param);
        if (!param_copy) {
            protocol_definition_free(copy);
            return NULL;
        }
        
        if (!copy->type_parameters) {
            copy->type_parameters = param_copy;
        } else {
            last_copy->next = param_copy;
        }
        last_copy = param_copy;
        param = param->next;
    }
    
    // Copy requirements
    if (protocol->requirement_count > 0) {
        copy->requirements = calloc(protocol->requirement_count, sizeof(ProtocolRequirement));
        if (!copy->requirements) {
            protocol_definition_free(copy);
            return NULL;
        }
        
        for (size_t i = 0; i < protocol->requirement_count; i++) {
            copy->requirements[i] = protocol_requirement_copy(&protocol->requirements[i]);
        }
        copy->requirement_count = protocol->requirement_count;
    }
    
    // Copy associated types
    if (protocol->associated_type_count > 0) {
        copy->associated_types = calloc(protocol->associated_type_count, sizeof(AssociatedType));
        if (!copy->associated_types) {
            protocol_definition_free(copy);
            return NULL;
        }
        
        for (size_t i = 0; i < protocol->associated_type_count; i++) {
            copy->associated_types[i] = associated_type_copy(&protocol->associated_types[i]);
        }
        copy->associated_type_count = protocol->associated_type_count;
    }
    
    // Copy super protocols (shallow copy)
    if (protocol->super_protocol_count > 0) {
        copy->super_protocols = calloc(protocol->super_protocol_count, sizeof(ProtocolDefinition*));
        if (!copy->super_protocols) {
            protocol_definition_free(copy);
            return NULL;
        }
        
        for (size_t i = 0; i < protocol->super_protocol_count; i++) {
            copy->super_protocols[i] = protocol->super_protocols[i];
        }
        copy->super_protocol_count = protocol->super_protocol_count;
    }
    
    copy->is_object_safe = protocol->is_object_safe;
    
    return copy;
}

// =============================================================================
// Protocol Requirement Management
// =============================================================================

ProtocolRequirement protocol_requirement_new_method(const char* name, Type* signature) {
    ProtocolRequirement req = {0};
    req.kind = PROTOCOL_REQ_METHOD;
    req.name = name ? strdup(name) : NULL;
    req.data.method.signature = signature;
    req.data.method.is_static = 0;
    req.data.method.is_mutating = 0;
    return req;
}

ProtocolRequirement protocol_requirement_new_property(const char* name, Type* prop_type, int has_getter, int has_setter) {
    ProtocolRequirement req = {0};
    req.kind = PROTOCOL_REQ_PROPERTY;
    req.name = name ? strdup(name) : NULL;
    req.data.property.type = prop_type;
    req.data.property.has_getter = has_getter;
    req.data.property.has_setter = has_setter;
    return req;
}

ProtocolRequirement protocol_requirement_new_init(Type* signature) {
    ProtocolRequirement req = {0};
    req.kind = PROTOCOL_REQ_INIT;
    req.name = strdup("init");
    req.data.initializer.signature = signature;
    req.data.initializer.is_required = 1;
    return req;
}

ProtocolRequirement protocol_requirement_new_associated_type(const char* name) {
    ProtocolRequirement req = {0};
    req.kind = PROTOCOL_REQ_ASSOCIATED_TYPE;
    req.name = name ? strdup(name) : NULL;
    // Associated type details handled separately
    return req;
}

ProtocolRequirement protocol_requirement_copy(const ProtocolRequirement* req) {
    if (!req) return (ProtocolRequirement){0};
    
    ProtocolRequirement copy = {0};
    copy.kind = req->kind;
    copy.name = req->name ? strdup(req->name) : NULL;
    
    switch (req->kind) {
        case PROTOCOL_REQ_METHOD:
            copy.data.method = req->data.method;
            // Note: Type signature is shared, not copied
            break;
            
        case PROTOCOL_REQ_PROPERTY:
            copy.data.property = req->data.property;
            // Note: Type is shared, not copied
            break;
            
        case PROTOCOL_REQ_INIT:
            copy.data.initializer = req->data.initializer;
            // Note: Type signature is shared, not copied
            break;
            
        case PROTOCOL_REQ_ASSOCIATED_TYPE:
            // No additional data to copy
            break;
    }
    
    return copy;
}

void protocol_requirement_free(ProtocolRequirement* req) {
    if (!req) return;
    
    free(req->name);
    // Note: Types are managed by the type system, not freed here
    memset(req, 0, sizeof(ProtocolRequirement));
}

// =============================================================================
// Associated Type Management
// =============================================================================

AssociatedType associated_type_new(const char* name, Type* default_type) {
    AssociatedType assoc = {0};
    assoc.name = name ? strdup(name) : NULL;
    assoc.default_type = default_type;
    assoc.constraints = NULL;
    assoc.constraint_count = 0;
    return assoc;
}

AssociatedType associated_type_copy(const AssociatedType* assoc) {
    if (!assoc) return (AssociatedType){0};
    
    AssociatedType copy = {0};
    copy.name = assoc->name ? strdup(assoc->name) : NULL;
    copy.default_type = assoc->default_type;  // Shallow copy
    
    // Copy constraints
    if (assoc->constraint_count > 0) {
        copy.constraints = calloc(assoc->constraint_count, sizeof(InterfaceConstraint));
        if (copy.constraints) {
            for (size_t i = 0; i < assoc->constraint_count; i++) {
                copy.constraints[i] = interface_constraint_copy(&assoc->constraints[i]);
            }
            copy.constraint_count = assoc->constraint_count;
        }
    }
    
    return copy;
}

void associated_type_free(AssociatedType* assoc) {
    if (!assoc) return;
    
    free(assoc->name);
    
    // Free constraints
    for (size_t i = 0; i < assoc->constraint_count; i++) {
        interface_constraint_free(&assoc->constraints[i]);
    }
    free(assoc->constraints);
    
    memset(assoc, 0, sizeof(AssociatedType));
}

// =============================================================================
// Protocol Conformance Management
// =============================================================================

ProtocolConformance* protocol_conformance_new(Type* conforming_type, ProtocolDefinition* protocol) {
    ProtocolConformance* conformance = calloc(1, sizeof(ProtocolConformance));
    if (!conformance) return NULL;
    
    conformance->conforming_type = conforming_type;
    conformance->protocol = protocol;
    conformance->method_implementations = NULL;
    conformance->associated_type_bindings = NULL;
    conformance->conditional_requirements = NULL;
    conformance->method_count = 0;
    conformance->associated_type_count = 0;
    conformance->conditional_count = 0;
    conformance->is_conditional = 0;
    conformance->is_synthesized = 0;
    
    return conformance;
}

void protocol_conformance_free(ProtocolConformance* conformance) {
    if (!conformance) return;
    
    // Free method implementations
    for (size_t i = 0; i < conformance->method_count; i++) {
        method_implementation_free(&conformance->method_implementations[i]);
    }
    free(conformance->method_implementations);
    
    // Free associated type bindings
    for (size_t i = 0; i < conformance->associated_type_count; i++) {
        associated_type_binding_free(&conformance->associated_type_bindings[i]);
    }
    free(conformance->associated_type_bindings);
    
    // Free conditional requirements
    for (size_t i = 0; i < conformance->conditional_count; i++) {
        interface_constraint_free(&conformance->conditional_requirements[i]);
    }
    free(conformance->conditional_requirements);
    
    free(conformance);
}

// =============================================================================
// Method Implementation Management
// =============================================================================

MethodImplementation method_implementation_new(const char* name, ASTNode* body) {
    MethodImplementation impl = {0};
    impl.method_name = name ? strdup(name) : NULL;
    impl.body = body;
    impl.signature = NULL;
    impl.is_default = 0;
    impl.is_override = 0;
    return impl;
}

MethodImplementation method_implementation_copy(const MethodImplementation* impl) {
    if (!impl) return (MethodImplementation){0};
    
    MethodImplementation copy = {0};
    copy.method_name = impl->method_name ? strdup(impl->method_name) : NULL;
    copy.body = impl->body;  // Shallow copy - AST is managed elsewhere
    copy.signature = impl->signature;  // Shallow copy - type is managed elsewhere
    copy.is_default = impl->is_default;
    copy.is_override = impl->is_override;
    
    return copy;
}

void method_implementation_free(MethodImplementation* impl) {
    if (!impl) return;
    
    free(impl->method_name);
    // Note: body and signature are managed elsewhere
    memset(impl, 0, sizeof(MethodImplementation));
}

// =============================================================================
// Associated Type Binding Management
// =============================================================================

AssociatedTypeBinding associated_type_binding_new(const char* name, Type* bound_type) {
    AssociatedTypeBinding binding = {0};
    binding.associated_type_name = name ? strdup(name) : NULL;
    binding.bound_type = bound_type;
    return binding;
}

AssociatedTypeBinding associated_type_binding_copy(const AssociatedTypeBinding* binding) {
    if (!binding) return (AssociatedTypeBinding){0};
    
    AssociatedTypeBinding copy = {0};
    copy.associated_type_name = binding->associated_type_name ? strdup(binding->associated_type_name) : NULL;
    copy.bound_type = binding->bound_type;  // Shallow copy
    
    return copy;
}

void associated_type_binding_free(AssociatedTypeBinding* binding) {
    if (!binding) return;
    
    free(binding->associated_type_name);
    // Note: bound_type is managed by type system
    memset(binding, 0, sizeof(AssociatedTypeBinding));
}

// =============================================================================
// Default Implementation Management
// =============================================================================

DefaultImplementation default_implementation_new(const char* method_name, ASTNode* body) {
    DefaultImplementation impl = {0};
    impl.method_name = method_name ? strdup(method_name) : NULL;
    impl.body = body;
    impl.signature = NULL;
    impl.constraints = NULL;
    impl.constraint_count = 0;
    return impl;
}

DefaultImplementation default_implementation_copy(const DefaultImplementation* impl) {
    if (!impl) return (DefaultImplementation){0};
    
    DefaultImplementation copy = {0};
    copy.method_name = impl->method_name ? strdup(impl->method_name) : NULL;
    copy.body = impl->body;  // Shallow copy
    copy.signature = impl->signature;  // Shallow copy
    
    // Copy constraints
    if (impl->constraint_count > 0) {
        copy.constraints = calloc(impl->constraint_count, sizeof(InterfaceConstraint));
        if (copy.constraints) {
            for (size_t i = 0; i < impl->constraint_count; i++) {
                copy.constraints[i] = interface_constraint_copy(&impl->constraints[i]);
            }
            copy.constraint_count = impl->constraint_count;
        }
    }
    
    return copy;
}

void default_implementation_free(DefaultImplementation* impl) {
    if (!impl) return;
    
    free(impl->method_name);
    
    // Free constraints
    for (size_t i = 0; i < impl->constraint_count; i++) {
        interface_constraint_free(&impl->constraints[i]);
    }
    free(impl->constraints);
    
    memset(impl, 0, sizeof(DefaultImplementation));
}

// =============================================================================
// Protocol System Integration
// =============================================================================

// Check if a type conforms to a protocol
int type_conforms_to_protocol(Type* type, ProtocolDefinition* protocol, TypeChecker* checker) {
    if (!type || !protocol || !checker) return 0;
    
    // Look for existing conformance
    ProtocolConformance* conformance = find_protocol_conformance(type, protocol, checker);
    if (conformance) {
        return validate_protocol_conformance(conformance, checker);
    }
    
    // Check for automatic conformance (e.g., built-in types)
    return check_automatic_protocol_conformance(type, protocol, checker);
}

// Find protocol conformance for a type
ProtocolConformance* find_protocol_conformance(Type* type, ProtocolDefinition* protocol, TypeChecker* checker) {
    if (!type || !protocol || !checker) return NULL;
    
    // This would integrate with the type checker's conformance table
    // For now, return NULL - this would be implemented as part of type checker integration
    return NULL;
}

// Validate that a conformance satisfies all protocol requirements
int validate_protocol_conformance(ProtocolConformance* conformance, TypeChecker* checker) {
    if (!conformance || !checker) return 0;
    
    ProtocolDefinition* protocol = conformance->protocol;
    if (!protocol) return 0;
    
    // Check all method requirements
    for (size_t i = 0; i < protocol->requirement_count; i++) {
        ProtocolRequirement* req = &protocol->requirements[i];
        
        if (req->kind == PROTOCOL_REQ_METHOD) {
            // Find corresponding method implementation
            MethodImplementation* impl = find_method_implementation(conformance, req->name);
            if (!impl) {
                // Check for default implementation
                DefaultImplementation* default_impl = find_default_implementation(protocol, req->name);
                if (!default_impl) {
                    return 0;  // Missing required method
                }
            } else {
                // Validate method signature matches requirement
                if (!type_signatures_compatible(impl->signature, req->data.method.signature, checker)) {
                    return 0;  // Signature mismatch
                }
            }
        } else if (req->kind == PROTOCOL_REQ_ASSOCIATED_TYPE) {
            // Check associated type binding
            AssociatedTypeBinding* binding = find_associated_type_binding(conformance, req->name);
            if (!binding) {
                // Check for default associated type
                AssociatedType* assoc = find_associated_type_definition(protocol, req->name);
                if (!assoc || !assoc->default_type) {
                    return 0;  // Missing associated type binding
                }
            }
        }
    }
    
    // Check conditional requirements if any
    for (size_t i = 0; i < conformance->conditional_count; i++) {
        InterfaceConstraint* constraint = &conformance->conditional_requirements[i];
        if (!interface_constraint_is_satisfied(constraint, conformance->conforming_type, checker)) {
            return 0;  // Conditional requirement not satisfied
        }
    }
    
    return 1;  // All requirements satisfied
}

// Check for automatic protocol conformance (e.g., built-in types)
int check_automatic_protocol_conformance(Type* type, ProtocolDefinition* protocol, TypeChecker* checker) {
    if (!type || !protocol || !checker) return 0;
    
    // Example: Numeric protocol for built-in numeric types
    if (protocol->name && strcmp(protocol->name, "Numeric") == 0) {
        return type_is_numeric(type);
    }
    
    // Example: Equatable protocol for types with == operator
    if (protocol->name && strcmp(protocol->name, "Equatable") == 0) {
        return type_supports_equality(type, checker);
    }
    
    // Example: Copyable protocol for value types
    if (protocol->name && strcmp(protocol->name, "Copyable") == 0) {
        return type_is_copyable(type);
    }
    
    return 0;
}

// =============================================================================
// Helper Functions
// =============================================================================

// Find method implementation in conformance
MethodImplementation* find_method_implementation(ProtocolConformance* conformance, const char* method_name) {
    if (!conformance || !method_name) return NULL;
    
    for (size_t i = 0; i < conformance->method_count; i++) {
        MethodImplementation* impl = &conformance->method_implementations[i];
        if (impl->method_name && strcmp(impl->method_name, method_name) == 0) {
            return impl;
        }
    }
    
    return NULL;
}

// Find default implementation in protocol
DefaultImplementation* find_default_implementation(ProtocolDefinition* protocol, const char* method_name) {
    if (!protocol || !method_name) return NULL;
    
    for (size_t i = 0; i < protocol->default_impl_count; i++) {
        DefaultImplementation* impl = &protocol->default_implementations[i];
        if (impl->method_name && strcmp(impl->method_name, method_name) == 0) {
            return impl;
        }
    }
    
    return NULL;
}

// Find associated type binding in conformance
AssociatedTypeBinding* find_associated_type_binding(ProtocolConformance* conformance, const char* assoc_name) {
    if (!conformance || !assoc_name) return NULL;
    
    for (size_t i = 0; i < conformance->associated_type_count; i++) {
        AssociatedTypeBinding* binding = &conformance->associated_type_bindings[i];
        if (binding->associated_type_name && strcmp(binding->associated_type_name, assoc_name) == 0) {
            return binding;
        }
    }
    
    return NULL;
}

// Find associated type definition in protocol
AssociatedType* find_associated_type_definition(ProtocolDefinition* protocol, const char* assoc_name) {
    if (!protocol || !assoc_name) return NULL;
    
    for (size_t i = 0; i < protocol->associated_type_count; i++) {
        AssociatedType* assoc = &protocol->associated_types[i];
        if (assoc->name && strcmp(assoc->name, assoc_name) == 0) {
            return assoc;
        }
    }
    
    return NULL;
}

// Check if type signatures are compatible
int type_signatures_compatible(Type* impl_sig, Type* req_sig, TypeChecker* checker) {
    if (!impl_sig || !req_sig || !checker) return 0;
    
    // This would use the existing type compatibility checking from the type system
    return types_are_compatible(impl_sig, req_sig, checker);
}

// Check if type is numeric
int type_is_numeric(Type* type) {
    if (!type) return 0;
    
    switch (type->kind) {
        case TYPE_INT:
        case TYPE_INT8:
        case TYPE_INT16:
        case TYPE_INT32:
        case TYPE_INT64:
        case TYPE_UINT:
        case TYPE_UINT8:
        case TYPE_UINT16:
        case TYPE_UINT32:
        case TYPE_UINT64:
        case TYPE_FLOAT:
        case TYPE_FLOAT32:
        case TYPE_FLOAT64:
            return 1;
        default:
            return 0;
    }
}

// Check if type supports equality comparison
int type_supports_equality(Type* type, TypeChecker* checker) {
    if (!type || !checker) return 0;
    
    // This would check if the type has == and != operators defined
    // For now, return true for basic types
    switch (type->kind) {
        case TYPE_INT:
        case TYPE_INT8:
        case TYPE_INT16:
        case TYPE_INT32:
        case TYPE_INT64:
        case TYPE_UINT:
        case TYPE_UINT8:
        case TYPE_UINT16:
        case TYPE_UINT32:
        case TYPE_UINT64:
        case TYPE_FLOAT:
        case TYPE_FLOAT32:
        case TYPE_FLOAT64:
        case TYPE_BOOL:
        case TYPE_CHAR:
        case TYPE_STRING:
            return 1;
        default:
            return 0;
    }
}

// Check if type is copyable
int type_is_copyable(Type* type) {
    if (!type) return 0;
    
    // Most value types are copyable by default
    switch (type->kind) {
        case TYPE_INT:
        case TYPE_INT8:
        case TYPE_INT16:
        case TYPE_INT32:
        case TYPE_INT64:
        case TYPE_UINT:
        case TYPE_UINT8:
        case TYPE_UINT16:
        case TYPE_UINT32:
        case TYPE_UINT64:
        case TYPE_FLOAT:
        case TYPE_FLOAT32:
        case TYPE_FLOAT64:
        case TYPE_BOOL:
        case TYPE_CHAR:
        case TYPE_STRING:
        case TYPE_ARRAY:
        case TYPE_STRUCT:
        case TYPE_ENUM:
            return 1;
        case TYPE_POINTER:
        case TYPE_REFERENCE:
            return 0;  // Pointers and references are not copyable by default
        default:
            return 0;
    }
}

// =============================================================================
// Standard Protocol Definitions
// =============================================================================

// Register standard protocols with the type system
void register_standard_protocols(TypeChecker* checker) {
    if (!checker) return;
    
    // Register Equatable protocol
    ProtocolDefinition* equatable = protocol_definition_new("Equatable", (Position){0, 0, ""});
    if (equatable) {
        // Add == method requirement
        Type* bool_type = type_checker_get_builtin(checker, TYPE_BOOL);
        Type* self_type = type_new(TYPE_SELF);
        Type* eq_signature = type_new_function(bool_type, &self_type, 1);
        
        ProtocolRequirement eq_req = protocol_requirement_new_method("==", eq_signature);
        
        equatable->requirements = malloc(sizeof(ProtocolRequirement));
        equatable->requirements[0] = eq_req;
        equatable->requirement_count = 1;
        
        // Register with type checker
        // type_checker_register_protocol(checker, equatable);
    }
    
    // Register Comparable protocol
    ProtocolDefinition* comparable = protocol_definition_new("Comparable", (Position){0, 0, ""});
    if (comparable) {
        // Inherit from Equatable
        comparable->super_protocols = malloc(sizeof(ProtocolDefinition*));
        comparable->super_protocols[0] = equatable;
        comparable->super_protocol_count = 1;
        
        // Add < method requirement
        Type* bool_type = type_checker_get_builtin(checker, TYPE_BOOL);
        Type* self_type = type_new(TYPE_SELF);
        Type* lt_signature = type_new_function(bool_type, &self_type, 1);
        
        ProtocolRequirement lt_req = protocol_requirement_new_method("<", lt_signature);
        
        comparable->requirements = malloc(sizeof(ProtocolRequirement));
        comparable->requirements[0] = lt_req;
        comparable->requirement_count = 1;
        
        // Register with type checker
        // type_checker_register_protocol(checker, comparable);
    }
    
    // Register Numeric protocol
    ProtocolDefinition* numeric = protocol_definition_new("Numeric", (Position){0, 0, ""});
    if (numeric) {
        // Add arithmetic method requirements
        Type* self_type = type_new(TYPE_SELF);
        
        ProtocolRequirement add_req = protocol_requirement_new_method("+", 
            type_new_function(self_type, &self_type, 1));
        ProtocolRequirement sub_req = protocol_requirement_new_method("-", 
            type_new_function(self_type, &self_type, 1));
        ProtocolRequirement mul_req = protocol_requirement_new_method("*", 
            type_new_function(self_type, &self_type, 1));
        ProtocolRequirement div_req = protocol_requirement_new_method("/", 
            type_new_function(self_type, &self_type, 1));
        
        numeric->requirements = malloc(4 * sizeof(ProtocolRequirement));
        numeric->requirements[0] = add_req;
        numeric->requirements[1] = sub_req;
        numeric->requirements[2] = mul_req;
        numeric->requirements[3] = div_req;
        numeric->requirement_count = 4;
        
        // Register with type checker
        // type_checker_register_protocol(checker, numeric);
    }
    
    // Register Collection protocol with associated type
    ProtocolDefinition* collection = protocol_definition_new("Collection", (Position){0, 0, ""});
    if (collection) {
        // Add Element associated type
        AssociatedType element_assoc = associated_type_new("Element", NULL);
        
        collection->associated_types = malloc(sizeof(AssociatedType));
        collection->associated_types[0] = element_assoc;
        collection->associated_type_count = 1;
        
        // Add count property requirement
        Type* int_type = type_checker_get_builtin(checker, TYPE_INT);
        ProtocolRequirement count_req = protocol_requirement_new_property("count", int_type, 1, 0);
        
        collection->requirements = malloc(sizeof(ProtocolRequirement));
        collection->requirements[0] = count_req;
        collection->requirement_count = 1;
        
        // Register with type checker
        // type_checker_register_protocol(checker, collection);
    }
}

// =============================================================================
// Utility Functions
// =============================================================================

const char* protocol_requirement_kind_to_string(ProtocolRequirementKind kind) {
    switch (kind) {
        case PROTOCOL_REQ_METHOD: return "method";
        case PROTOCOL_REQ_PROPERTY: return "property";
        case PROTOCOL_REQ_INIT: return "initializer";
        case PROTOCOL_REQ_ASSOCIATED_TYPE: return "associated_type";
        default: return "unknown";
    }
}

void print_protocol_definition(const ProtocolDefinition* protocol) {
    if (!protocol) {
        printf("(null protocol)\n");
        return;
    }
    
    printf("Protocol %s {\n", protocol->name ? protocol->name : "(unnamed)");
    
    // Print super protocols
    if (protocol->super_protocol_count > 0) {
        printf("  inherits: ");
        for (size_t i = 0; i < protocol->super_protocol_count; i++) {
            printf("%s", protocol->super_protocols[i]->name ? protocol->super_protocols[i]->name : "(unnamed)");
            if (i < protocol->super_protocol_count - 1) printf(", ");
        }
        printf("\n");
    }
    
    // Print associated types
    for (size_t i = 0; i < protocol->associated_type_count; i++) {
        printf("  associatedtype %s\n", protocol->associated_types[i].name ? protocol->associated_types[i].name : "(unnamed)");
    }
    
    // Print requirements
    for (size_t i = 0; i < protocol->requirement_count; i++) {
        ProtocolRequirement* req = &protocol->requirements[i];
        printf("  %s %s\n", 
               protocol_requirement_kind_to_string(req->kind),
               req->name ? req->name : "(unnamed)");
    }
    
    printf("  object_safe: %s\n", protocol->is_object_safe ? "true" : "false");
    
    printf("}\n");
}

void print_protocol_conformance(const ProtocolConformance* conformance) {
    if (!conformance) {
        printf("(null conformance)\n");
        return;
    }
    
    printf("Conformance {\n");
    printf("  type: %s\n", conformance->conforming_type ? "(type)" : "(null)");
    printf("  protocol: %s\n", 
           conformance->protocol && conformance->protocol->name ? 
           conformance->protocol->name : "(null)");
    printf("  methods: %zu\n", conformance->method_count);
    printf("  associated_types: %zu\n", conformance->associated_type_count);
    printf("  is_conditional: %s\n", conformance->is_conditional ? "true" : "false");
    printf("  is_synthesized: %s\n", conformance->is_synthesized ? "true" : "false");
    printf("}\n");
}
