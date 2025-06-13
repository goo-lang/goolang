// Enhanced Interface System Integration with Type Checker
// Provides hooks to connect the new systems with existing infrastructure

#include "interface_system.h"
#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Forward declarations
void register_standard_concepts(TypeChecker* checker);
void register_standard_hkts(TypeChecker* checker);
void register_standard_protocols(TypeChecker* checker);

// Registry function declarations
ConceptRegistry* concept_registry_new(void);
void concept_registry_free(ConceptRegistry* registry);
ConceptDefinition* concept_registry_lookup(ConceptRegistry* registry, const char* name);
int concept_registry_register(ConceptRegistry* registry, ConceptDefinition* concept);

HKTRegistry* hkt_registry_new(void);
void hkt_registry_free(HKTRegistry* registry);
HigherKindedType* hkt_registry_lookup(HKTRegistry* registry, const char* name);
int hkt_registry_register(HKTRegistry* registry, HigherKindedType* hkt);

ProtocolRegistry* protocol_registry_new(void);
void protocol_registry_free(ProtocolRegistry* registry);
ProtocolDefinition* protocol_registry_lookup(ProtocolRegistry* registry, const char* name);
int protocol_registry_register(ProtocolRegistry* registry, ProtocolDefinition* protocol);

// =============================================================================
// Type Checker Integration Hooks
// =============================================================================

// Initialize enhanced interface system in type checker
int type_checker_init_enhanced_interfaces(TypeChecker* checker) {
    if (!checker) return 0;
    
    // Initialize constraint inference engine
    checker->constraint_engine = constraint_inference_engine_new(checker);
    if (!checker->constraint_engine) return 0;
    
    // Initialize concept registry
    checker->concept_registry = concept_registry_new();
    if (!checker->concept_registry) {
        constraint_inference_engine_free(checker->constraint_engine);
        checker->constraint_engine = NULL;
        return 0;
    }
    
    // Initialize HKT registry
    checker->hkt_registry = hkt_registry_new();
    if (!checker->hkt_registry) {
        constraint_inference_engine_free(checker->constraint_engine);
        concept_registry_free(checker->concept_registry);
        checker->constraint_engine = NULL;
        checker->concept_registry = NULL;
        return 0;
    }
    
    // Initialize protocol registry
    checker->protocol_registry = protocol_registry_new();
    if (!checker->protocol_registry) {
        constraint_inference_engine_free(checker->constraint_engine);
        concept_registry_free(checker->concept_registry);
        hkt_registry_free(checker->hkt_registry);
        checker->constraint_engine = NULL;
        checker->concept_registry = NULL;
        checker->hkt_registry = NULL;
        return 0;
    }
    
    // Register standard concepts and protocols
    register_standard_concepts(checker);
    register_standard_hkts(checker);
    register_standard_protocols(checker);
    
    return 1;
}

// Cleanup enhanced interface system
void type_checker_cleanup_enhanced_interfaces(TypeChecker* checker) {
    if (!checker) return;
    
    if (checker->constraint_engine) {
        constraint_inference_engine_free(checker->constraint_engine);
        checker->constraint_engine = NULL;
    }
    
    if (checker->concept_registry) {
        concept_registry_free(checker->concept_registry);
        checker->concept_registry = NULL;
    }
    
    if (checker->hkt_registry) {
        hkt_registry_free(checker->hkt_registry);
        checker->hkt_registry = NULL;
    }
    
    if (checker->protocol_registry) {
        protocol_registry_free(checker->protocol_registry);
        checker->protocol_registry = NULL;
    }
}

// =============================================================================
// Constraint Inference Integration
// =============================================================================

// Infer constraints from expression during type checking
InterfaceConstraint* type_checker_infer_expression_constraints(TypeChecker* checker, ASTNode* expr) {
    if (!checker || !expr || !checker->constraint_engine) return NULL;
    
    // The function returns an int, so we need to handle this differently
    int result = infer_constraints_from_expression(checker->constraint_engine, expr);
    
    // For now, return NULL to indicate no constraints inferred
    // In a full implementation, this would return actual constraints
    (void)result; // Suppress unused warning
    return NULL;
}

// Solve constraints for generic function instantiation
int type_checker_solve_constraints(TypeChecker* checker, TypeVariable** variables, size_t var_count) {
    if (!checker || !variables || !checker->constraint_engine) return 0;
    
    // The solve_constraints function only takes the engine parameter
    (void)variables; // Suppress unused warning
    (void)var_count; // Suppress unused warning
    return solve_constraints(checker->constraint_engine);
}

// =============================================================================
// Concept-Based Generics Integration
// =============================================================================

// Check if type satisfies concept during type checking
int type_checker_check_concept_satisfaction(TypeChecker* checker, Type* type, const char* concept_name) {
    if (!checker || !type || !concept_name || !checker->concept_registry) return 0;
    
    ConceptDefinition* concept = concept_registry_lookup(checker->concept_registry, concept_name);
    if (!concept) return 0;
    
    return type_satisfies_concept(type, concept, checker);
}

// Instantiate generic function with concept constraints
Type* type_checker_instantiate_generic_with_concepts(TypeChecker* checker, Type* generic_func, Type** arg_types, size_t arg_count) {
    if (!checker || !generic_func || !arg_types) return NULL;
    
    // For now, return the original function type
    // In a full implementation, this would instantiate with concepts
    (void)arg_types;
    (void)arg_count;
    return generic_func;
}

// =============================================================================
// Higher-Kinded Types Integration
// =============================================================================

// Apply higher-kinded type during type checking
Type* type_checker_apply_higher_kinded_type(TypeChecker* checker, const char* hkt_name, Type** args, size_t arg_count) {
    if (!checker || !hkt_name || !args || !checker->hkt_registry) return NULL;
    
    HigherKindedType* hkt = hkt_registry_lookup(checker->hkt_registry, hkt_name);
    if (!hkt) return NULL;
    
    // For now, return NULL - this would be implemented with actual HKT application
    (void)arg_count;
    return NULL;
}

// Validate HKT application during type checking
int type_checker_validate_hkt_application(TypeChecker* checker, const char* hkt_name, size_t arg_count) {
    if (!checker || !hkt_name || !checker->hkt_registry) return 0;
    
    HigherKindedType* hkt = hkt_registry_lookup(checker->hkt_registry, hkt_name);
    if (!hkt) return 0;
    
    // For now, return success - this would validate arity and kinds
    (void)arg_count;
    return 1;
}

// =============================================================================
// Type-Level Programming Integration
// =============================================================================

// Apply type-level computation during type checking
Type* type_checker_apply_type_level_computation(TypeChecker* checker, TypeLevelComputation* comp, Type** args, size_t arg_count) {
    if (!checker || !comp || !args) return NULL;
    
    // For now, return NULL - this would evaluate the computation
    (void)arg_count;
    return NULL;
}

// Validate dependent type during type checking
int type_checker_validate_dependent_type(TypeChecker* checker, Type* dep_type) {
    if (!checker || !dep_type) return 0;
    
    // Check if this is actually a dependent type
    if (dep_type->kind != TYPE_INTERFACE) return 0; // Using existing type kind
    
    // For now, return success - this would validate dependencies
    return 1;
}

// =============================================================================
// Protocol-Oriented Programming Integration
// =============================================================================

// Check protocol conformance during type checking
int type_checker_check_protocol_conformance(TypeChecker* checker, Type* type, const char* protocol_name) {
    if (!checker || !type || !protocol_name || !checker->protocol_registry) return 0;
    
    ProtocolDefinition* protocol = protocol_registry_lookup(checker->protocol_registry, protocol_name);
    if (!protocol) return 0;
    
    // For now, return success - this would check actual conformance
    return 1;
}

// Register protocol conformance for type
int type_checker_register_protocol_conformance(TypeChecker* checker, Type* type, const char* protocol_name, ProtocolConformance* conformance) {
    if (!checker || !type || !protocol_name || !conformance || !checker->protocol_registry) return 0;
    
    // For now, return success - this would register the conformance
    return 1;
}

// Lookup associated type from protocol conformance
Type* type_checker_lookup_associated_type(TypeChecker* checker, Type* type, const char* assoc_name) {
    if (!checker || !type || !assoc_name) return NULL;
    
    // For now, return NULL - this would lookup the associated type
    return NULL;
}

// =============================================================================
// Registry Management Functions
// =============================================================================

// Concept Registry Functions
ConceptRegistry* concept_registry_new(void) {
    ConceptRegistry* registry = malloc(sizeof(ConceptRegistry));
    if (!registry) return NULL;
    
    registry->concepts = malloc(sizeof(ConceptDefinition*) * 16);
    if (!registry->concepts) {
        free(registry);
        return NULL;
    }
    
    registry->concept_count = 0;
    registry->capacity = 16;
    return registry;
}

void concept_registry_free(ConceptRegistry* registry) {
    if (!registry) return;
    
    for (size_t i = 0; i < registry->concept_count; i++) {
        concept_definition_free(registry->concepts[i]);
    }
    free(registry->concepts);
    free(registry);
}

ConceptDefinition* concept_registry_lookup(ConceptRegistry* registry, const char* name) {
    if (!registry || !name) return NULL;
    
    for (size_t i = 0; i < registry->concept_count; i++) {
        if (strcmp(registry->concepts[i]->name, name) == 0) {
            return registry->concepts[i];
        }
    }
    return NULL;
}

int concept_registry_register(ConceptRegistry* registry, ConceptDefinition* concept) {
    if (!registry || !concept) return 0;
    
    // Check if we need to resize
    if (registry->concept_count >= registry->capacity) {
        size_t new_capacity = registry->capacity * 2;
        ConceptDefinition** new_concepts = realloc(registry->concepts, 
            sizeof(ConceptDefinition*) * new_capacity);
        if (!new_concepts) return 0;
        
        registry->concepts = new_concepts;
        registry->capacity = new_capacity;
    }
    
    registry->concepts[registry->concept_count++] = concept;
    return 1;
}

// HKT Registry Functions
HKTRegistry* hkt_registry_new(void) {
    HKTRegistry* registry = malloc(sizeof(HKTRegistry));
    if (!registry) return NULL;
    
    registry->hkts = malloc(sizeof(HigherKindedType*) * 16);
    if (!registry->hkts) {
        free(registry);
        return NULL;
    }
    
    registry->hkt_count = 0;
    registry->capacity = 16;
    return registry;
}

void hkt_registry_free(HKTRegistry* registry) {
    if (!registry) return;
    
    for (size_t i = 0; i < registry->hkt_count; i++) {
        higher_kinded_type_free(registry->hkts[i]);
    }
    free(registry->hkts);
    free(registry);
}

HigherKindedType* hkt_registry_lookup(HKTRegistry* registry, const char* name) {
    if (!registry || !name) return NULL;
    
    for (size_t i = 0; i < registry->hkt_count; i++) {
        if (strcmp(registry->hkts[i]->name, name) == 0) {
            return registry->hkts[i];
        }
    }
    return NULL;
}

int hkt_registry_register(HKTRegistry* registry, HigherKindedType* hkt) {
    if (!registry || !hkt) return 0;
    
    // Check if we need to resize
    if (registry->hkt_count >= registry->capacity) {
        size_t new_capacity = registry->capacity * 2;
        HigherKindedType** new_hkts = realloc(registry->hkts, 
            sizeof(HigherKindedType*) * new_capacity);
        if (!new_hkts) return 0;
        
        registry->hkts = new_hkts;
        registry->capacity = new_capacity;
    }
    
    registry->hkts[registry->hkt_count++] = hkt;
    return 1;
}

// Protocol Registry Functions
ProtocolRegistry* protocol_registry_new(void) {
    ProtocolRegistry* registry = malloc(sizeof(ProtocolRegistry));
    if (!registry) return NULL;
    
    registry->protocols = malloc(sizeof(ProtocolDefinition*) * 16);
    if (!registry->protocols) {
        free(registry);
        return NULL;
    }
    
    registry->conformances = malloc(sizeof(ProtocolConformance*) * 16);
    if (!registry->conformances) {
        free(registry->protocols);
        free(registry);
        return NULL;
    }
    
    registry->protocol_count = 0;
    registry->conformance_count = 0;
    registry->protocol_capacity = 16;
    registry->conformance_capacity = 16;
    return registry;
}

void protocol_registry_free(ProtocolRegistry* registry) {
    if (!registry) return;
    
    for (size_t i = 0; i < registry->protocol_count; i++) {
        protocol_definition_free(registry->protocols[i]);
    }
    
    for (size_t i = 0; i < registry->conformance_count; i++) {
        protocol_conformance_free(registry->conformances[i]);
    }
    
    free(registry->protocols);
    free(registry->conformances);
    free(registry);
}

ProtocolDefinition* protocol_registry_lookup(ProtocolRegistry* registry, const char* name) {
    if (!registry || !name) return NULL;
    
    for (size_t i = 0; i < registry->protocol_count; i++) {
        if (strcmp(registry->protocols[i]->name, name) == 0) {
            return registry->protocols[i];
        }
    }
    return NULL;
}

int protocol_registry_register(ProtocolRegistry* registry, ProtocolDefinition* protocol) {
    if (!registry || !protocol) return 0;
    
    // Check if we need to resize
    if (registry->protocol_count >= registry->protocol_capacity) {
        size_t new_capacity = registry->protocol_capacity * 2;
        ProtocolDefinition** new_protocols = realloc(registry->protocols, 
            sizeof(ProtocolDefinition*) * new_capacity);
        if (!new_protocols) return 0;
        
        registry->protocols = new_protocols;
        registry->protocol_capacity = new_capacity;
    }
    
    registry->protocols[registry->protocol_count++] = protocol;
    return 1;
}

// =============================================================================
// Standard Library Registration
// =============================================================================

void register_standard_concepts(TypeChecker* checker) {
    if (!checker || !checker->concept_registry) return;
    
    Position pos = {0, 0, 0, ""};
    
    // Create standard Numeric concept
    ConceptDefinition* numeric = concept_definition_new("Numeric", pos);
    if (numeric) {
        InterfaceConstraint* arithmetic_constraint = interface_constraint_new(CONSTRAINT_ARITHMETIC, NULL, pos);
        InterfaceConstraint* numeric_constraint = interface_constraint_new(CONSTRAINT_NUMERIC, NULL, pos);
        
        if (arithmetic_constraint && numeric_constraint) {
            concept_add_requirement(numeric, arithmetic_constraint);
            concept_add_requirement(numeric, numeric_constraint);
            concept_registry_register(checker->concept_registry, numeric);
        } else {
            if (arithmetic_constraint) interface_constraint_free(arithmetic_constraint);
            if (numeric_constraint) interface_constraint_free(numeric_constraint);
            concept_definition_free(numeric);
        }
    }
    
    // Create standard Equatable concept
    ConceptDefinition* equatable = concept_definition_new("Equatable", pos);
    if (equatable) {
        InterfaceConstraint* eq_constraint = interface_constraint_new(CONSTRAINT_PARTIAL_EQ, NULL, pos);
        if (eq_constraint) {
            concept_add_requirement(equatable, eq_constraint);
            concept_registry_register(checker->concept_registry, equatable);
        } else {
            concept_definition_free(equatable);
        }
    }
    
    // Create standard Comparable concept (depends on Equatable)
    ConceptDefinition* comparable = concept_definition_new("Comparable", pos);
    if (comparable && equatable) {
        concept_add_super_concept(comparable, equatable);
        
        InterfaceConstraint* ord_constraint = interface_constraint_new(CONSTRAINT_PARTIAL_ORD, NULL, pos);
        if (ord_constraint) {
            concept_add_requirement(comparable, ord_constraint);
            concept_registry_register(checker->concept_registry, comparable);
        } else {
            concept_definition_free(comparable);
        }
    }
}

void register_standard_hkts(TypeChecker* checker) {
    if (!checker || !checker->hkt_registry) return;
    
    Position pos = {0, 0, 0, ""};
    
    // For now, create simplified HKT registrations
    // In a full implementation, these would be properly constructed with type constructors
    
    // Register Option<T> - simplified version
    HigherKindedType* option_hkt = higher_kinded_type_new(HKT_TYPE_TO_TYPE, NULL);
    if (option_hkt) {
        // Set the name manually since we can't use the constructor with string
        option_hkt->name = strdup("Option");
        if (option_hkt->name) {
            hkt_registry_register(checker->hkt_registry, option_hkt);
        } else {
            higher_kinded_type_free(option_hkt);
        }
    }
    
    // Register Vec<T> - simplified version  
    HigherKindedType* vec_hkt = higher_kinded_type_new(HKT_TYPE_TO_TYPE, NULL);
    if (vec_hkt) {
        vec_hkt->name = strdup("Vec");
        if (vec_hkt->name) {
            hkt_registry_register(checker->hkt_registry, vec_hkt);
        } else {
            higher_kinded_type_free(vec_hkt);
        }
    }
}

void register_standard_protocols(TypeChecker* checker) {
    if (!checker || !checker->protocol_registry) return;
    
    Position pos = {0, 0, 0, ""};
    
    // Register Iterator protocol
    ProtocolDefinition* iterator_protocol = protocol_definition_new("Iterator", pos);
    if (iterator_protocol) {
        // For now, skip associated type registration - would need proper Type* creation
        // In a full implementation, this would add associated types properly
        protocol_registry_register(checker->protocol_registry, iterator_protocol);
    }
    
    // Register Display protocol
    ProtocolDefinition* display_protocol = protocol_definition_new("Display", pos);
    if (display_protocol) {
        protocol_registry_register(checker->protocol_registry, display_protocol);
    }
}

// =============================================================================
// Stub implementations for missing protocol functions
// =============================================================================

ProtocolDefinition* protocol_definition_new(const char* name, Position pos) {
    if (!name) return NULL;
    
    ProtocolDefinition* protocol = malloc(sizeof(ProtocolDefinition));
    if (!protocol) return NULL;
    
    protocol->name = strdup(name);
    protocol->type_parameters = NULL;
    protocol->required_methods = NULL;
    protocol->default_methods = NULL;
    protocol->associated_types = NULL;
    protocol->associated_type_count = 0;
    protocol->where_clause = NULL;
    protocol->inherited_protocols = NULL;
    protocol->inherited_count = 0;
    protocol->allows_retroactive_conformance = 1;
    protocol->is_auto_conformance = 0;
    protocol->defined_pos = pos;
    protocol->next = NULL;
    
    return protocol;
}

void protocol_definition_free(ProtocolDefinition* protocol) {
    if (!protocol) return;
    
    free(protocol->name);
    // Note: Other fields would need proper cleanup in full implementation
    free(protocol);
}

void protocol_conformance_free(ProtocolConformance* conformance) {
    if (!conformance) return;
    
    // Note: Fields would need proper cleanup in full implementation
    free(conformance);
}
